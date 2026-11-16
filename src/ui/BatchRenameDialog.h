// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/BatchRenameDialog.h
//
// 「批量重命名」窗口（选中多个文件/文件夹后右键进来）。
//
// 【界面结构】
//   上半部分：预览列表（原名 → 新名 → 状态），规则改一下列表立刻重算；
//   下半部分：规则区，两栏：
//       左栏  名称类：前缀 / 后缀 / 查找 / 替换为 / 区分大小写
//       右栏  加工类：序号（起始、步长、位数、位置、分隔符）/ 扩展名 / 大小写 / 排序
//   底部：左侧一行提示（共几项、改了几项、有没有冲突），右侧「取消 / 应用」两个自绘按钮。
//
// 【风格】
//   和「设置」「打开方式」两个窗口一套：白底无边框客户区，只有系统控件
//   （编辑框、复选框、下拉框、列表）用原生的，按钮自己画。
//
// 【本阶段做到哪】
//   先把界面和"能看见效果"的预览做出来：
//     · 规则全部就位，任何改动都会即时算出新名字并刷到右边；
//     · 状态列会标出"未改动 / 已修改 / 冲突（重名、非法字符、空名）"；
//     · 点「应用」暂时只把结果回传（真正改名、撤销、进度条等下一步接）。
// ============================================================================

#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

// ----------------------------------------------------------------------------
// 一条待改名的项
// ----------------------------------------------------------------------------
struct BatchRenameItem
{
    std::wstring path;        // 完整路径（改名前）
    std::wstring name;        // 原文件名（含扩展名）
    bool         isFolder = false;
    uint64_t     size     = 0;   // 文件大小（排序用）
    FILETIME     modified = {};  // 修改时间（排序用）
};

// ----------------------------------------------------------------------------
// 应用后回传的一条结果
// ----------------------------------------------------------------------------
struct BatchRenameResult
{
    std::wstring oldPath;
    std::wstring newName;   // 只含文件名，不含目录
};

class BatchRenameDialog
{
public:
    BatchRenameDialog() = default;
    ~BatchRenameDialog();

    BatchRenameDialog(const BatchRenameDialog&) = delete;
    BatchRenameDialog& operator=(const BatchRenameDialog&) = delete;

    // 以模态方式打开。
    // paths：要改名的项的完整路径（调用方保证至少 2 项、且都是文件系统路径）
    // 返回 true 表示用户点了「应用」，results 里是每一项的目标文件名；
    // 返回 false 表示取消。
    bool ShowModal(HWND owner, const std::vector<std::wstring>& paths,
                   std::vector<BatchRenameResult>& results);

private:
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    static bool EnsureClassRegistered();

    bool CreateDialogWindow();
    void Layout();
    void CreateControls();

    // 整窗背景 + 标题 + 分隔线 + 底部按钮
    void Draw(HDC dc);
    void DrawButton(HDC dc, const RECT& rect, const wchar_t* text, bool hot, bool pressed, bool primary);

    int Scale(int value) const;

    // 按当前规则算一遍新名字，并刷新右侧列表与底部统计
    void Recalculate();

    // 计算一项的新文件名（不含目录）
    std::wstring ComputeNewName(size_t index, const std::wstring& originalName) const;

    // 预览列表：填充所有行
    void FillPreviewList();

    // 状态文字与颜色（0 正常、1 已修改、2 冲突）
    struct PreviewRow
    {
        size_t       itemIndex = 0;   // 对应 m_items 里的下标（列表按排序显示，和这个不是一回事）
        std::wstring oldName;
        std::wstring newName;
        int          state = 0;       // 0 未改动 / 1 已修改 / 2 冲突
        std::wstring stateText;
    };

    // 读所有控件当前值，重算 rows
    void BuildPreviewRows();

    // 控件值读取小工具
    std::wstring GetTextOf(HWND control) const;
    int          ComboSelection(HWND control) const;

    void EnableRuleControls();

private:
    HWND m_hWnd   = nullptr;
    HWND m_hOwner = nullptr;

    std::vector<BatchRenameItem> m_items;
    std::vector<PreviewRow>      m_rows;

    // ---- 控件句柄 ----
    HWND m_hList = nullptr;         // 预览列表（report 模式的 ListView）

    HWND m_hPrefix = nullptr;       // 前缀
    HWND m_hSuffix = nullptr;       // 后缀
    HWND m_hFind = nullptr;         // 查找
    HWND m_hReplace = nullptr;      // 替换为
    HWND m_hCaseSensitive = nullptr; // 区分大小写（复选框）

    HWND m_hNumberEnable = nullptr; // 加序号（复选框）
    HWND m_hNumberStart = nullptr;  // 起始
    HWND m_hNumberStep = nullptr;   // 步长
    HWND m_hNumberDigits = nullptr; // 位数
    HWND m_hNumberPos = nullptr;    // 位置（前缀 / 后缀）
    HWND m_hNumberSep = nullptr;    // 分隔符

    HWND m_hExtMode = nullptr;      // 扩展名：保留 / 改为 / 删除
    HWND m_hExtValue = nullptr;     // 扩展名改成什么

    HWND m_hCaseMode = nullptr;     // 大小写：保持 / 大写 / 小写 / 首字母大写

    HWND m_hSortMode = nullptr;     // 排序：名称 / 修改时间 / 大小
    HWND m_hSortReverse = nullptr;  // 反向（复选框）

    // ---- 自绘按钮 ----
    RECT m_cancelRect = {};
    RECT m_applyRect  = {};
    bool m_hotCancel  = false;
    bool m_pressedCancel = false;
    bool m_hotApply   = false;
    bool m_pressedApply  = false;
    bool m_trackingLeave = false;

    // ---- 字体 ----
    HFONT m_font       = nullptr;
    HFONT m_fontTitle  = nullptr;
    HFONT m_fontSmall  = nullptr;   // 底部提示用
    HBRUSH m_whiteBrush = nullptr;

    // ---- 结果 ----
    bool m_applied = false;    // 用户点了「应用」
};
