// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/OverlaySettingsDialog.h
//
// 「同步状态角标」配置窗口（模态）。
//
// 界面三列：
//
//   ┌──────────┬────────────┬──────────────────────────────────────────┐
//   │ 优先级    │ 应用名      │ 显示范围                                   │
//   ├──────────┼────────────┼──────────────────────────────────────────┤
//   │ 1  ↑ ↓   │ Tortoise   │ ☑ 处理全部路径                             │
//   │ 2  ↑ ↓   │ OneDrive   │ ☐ [git ×] [网盘 ×] [+]                    │
//   └──────────┴────────────┴──────────────────────────────────────────┘
//
//   · 优先级：序号 + ↑ / ↓ 按钮，上下移动调整先后顺序（不做拖拽）；
//   · 应用名：一套处理器一行（同族合并）；
//   · 显示范围：复选框勾上 = 处理全部路径；不勾 = 白名单，末尾「+」添加目录，
//     路径做成可删的小按钮（chip），悬停显示完整路径，过窄时省略文件名只留 ×；
//     不勾且路径为空 = 这套禁用。
//
// 【一期界面阶段】套列表还没接真实注册表枚举，先用占位数据填充；
// 交互（勾选、↑↓、chip 增删、悬停）先做通，确定界面后再接处理器。
//
// 【即改即存】改动记在工作副本里，点「确定」一次性写回配置；「取消」丢弃。
// ============================================================================

#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "core/Settings.h"
#include "ui/CustomWindow.h"

namespace ui
{

class OverlaySettingsDialog : public CustomWindow
{
public:
    explicit OverlaySettingsDialog(HWND owner);
    ~OverlaySettingsDialog();

    OverlaySettingsDialog(const OverlaySettingsDialog&) = delete;
    OverlaySettingsDialog& operator=(const OverlaySettingsDialog&) = delete;

    // 显示模态窗口。返回 true＝用户点了「确定」（配置已写回）。
    bool ShowModal();

private:
    LRESULT HandleMessage(HWND, UINT msg, WPARAM wParam, LPARAM lParam) override;

    static bool EnsureClassRegistered();

    bool CreateDialogWindow();
    void CreateControls();
    void Layout();
    void Draw(HDC dc);
    void DrawRow(HDC dc, const RECT& rowRect, int index, bool selected);
    void DrawCheckBox(HDC dc, const RECT& box, bool checked);

    // 工作副本 → 配置文件
    void SaveNow();

    // 工作副本 → 列表控件
    void ReloadList();

    // 行内可点区域（Layout / DrawRow 算好，NM_CLICK 命中用）
    struct ChipHit
    {
        RECT chip  = {};   // 整个 chip 矩形（悬停变色用）
        RECT close = {};   // 右侧 × 关闭区（只有点这里才删除）
    };

    struct RowHit
    {
        RECT up    = {};   // ↑ 按钮
        RECT down  = {};   // ↓ 按钮
        RECT check = {};   // 显示范围复选框
        RECT add   = {};   // 末尾「+」按钮
        std::vector<ChipHit> chips;   // 每个路径 chip（与 paths 一一对应）
    };

    std::vector<RowHit> m_hits;

    void MoveSet(int index, int delta);   // ↑↓ 调整顺序
    void ToggleScope(int index);           // 勾选全部 / 切到白名单
    void AddPath(int index);               // 「+」选目录加入白名单
    void RemovePath(int index, int chip);  // chip 上的 × 删除

    // 行内按钮悬停跟踪（子类化列表控件后由鼠标消息驱动）
    enum HotPart { HotNone = 0, HotUp = 1, HotDown = 2, HotAdd = 3, HotChip = 4 };
    int  m_hotRow  = -1;   // 悬停在第几行（-1 = 无）
    int  m_hotPart = HotNone; // 悬停该行的哪个按钮
    int  m_hotChip = -1;  // 悬停该行第几个 chip（仅 HotChip 时有效）
    void UpdateRowHover(int x, int y);     // 根据列表客户区坐标刷新悬停态

    // chip 完整路径 tooltip（原生 ToolTip 控件，悬停 chip 时弹出）
    HWND  m_hTooltip = nullptr;
    std::wstring m_tipText;                // 当前 tooltip 文本（完整路径）
    void UpdateTooltip(int row, int chip); // 按命中的 chip 刷新/隐藏 tooltip

    static LRESULT CALLBACK ListSubclassProc(HWND hWnd, UINT msg, WPARAM wParam,
                                             LPARAM lParam, UINT_PTR id, DWORD_PTR refData);

    // 底部按钮：确定 / 取消
    enum Button
    {
        ButtonOk = 0,
        ButtonCancel,
        ButtonCount
    };

    RECT  m_buttonRects[ButtonCount] = {};
    int   m_hotButton    = -1;
    int   m_pressedButton = -1;

    HWND  m_hOwner = nullptr;
    HWND  m_hList  = nullptr;
    HFONT m_font   = nullptr;

    std::vector<core::OverlaySet> m_sets;   // 工作副本
    bool  m_confirmed = false;              // 是否点了「确定」

    RECT  m_listRect = {};
};

} // namespace ui
