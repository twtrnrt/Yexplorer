// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/ContextMenuDialog.h
//
// 「自定义右键菜单」配置窗口。
//
// 界面：
//   ┌──────────────────────────────────────────────────────────────┐
//   │ 场景 [单选文件 ▾]   勾选要显示的项；列表顺序就是菜单顺序      │
//   ├────────────────────────────────┬─────────────────────────────┤
//   │ ☑ 打开                         │ 快捷动作（可选）             │
//   │ ☑ 打开方式                     │  用 VS Code 打开             │
//   │ ──── 分隔线 ────               │  7-Zip 解压到当前文件夹       │
//   │ ☑ 剪切 / 复制 / 复制文件路径    │                             │
//   │ ...                            │ [添加程序…] [预设▾] [删除]   │
//   ├────────────────────────────────┴─────────────────────────────┤
//   │ [上移] [下移] [加分隔线] [恢复默认]          [保存] [取消]   │
//   └──────────────────────────────────────────────────────────────┘
//
// 说明：
//   · 列表里始终列出当前场景的**全部**内置项；不勾选 = 隐藏。
//   · 分隔线也是一行，可勾选/取消/移动；连续分隔线保存时自动合并。
//   · 「快捷动作」是整组开关；具体动作在右栏添加/删除/排序。
//   · 只读写 %APPDATA%\FileExplorer\context-menu.json，不碰注册表/COM。
// ============================================================================

#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "ui/CustomWindow.h"
#include "ui/MenuLayout.h"

namespace ui
{

class ContextMenuDialog : public CustomWindow
{
public:
    explicit ContextMenuDialog(HWND owner);
    ~ContextMenuDialog();

    ContextMenuDialog(const ContextMenuDialog&) = delete;
    ContextMenuDialog& operator=(const ContextMenuDialog&) = delete;

    // 返回 true 表示点了「保存」（配置已写入并让缓存失效）
    bool ShowModal();

private:
    // 列表里的一行
    struct Row
    {
        enum class Kind { Item, Separator, Actions };
        Kind         kind = Kind::Item;
        std::wstring id;      // Item 的内置项 ID；其它为空
        std::wstring label;
        bool         checked = true;
    };

    LRESULT HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) override;

    static bool EnsureClassRegistered();

    bool CreateDialogWindow();
    void CreateControls();
    void Layout();
    void DrawHint(HDC dc);

    void ReloadScene();          // 配置 → 行
    void ReloadList();           // 行 → ListView
    void CommitList();           // ListView → 配置（当前场景）
    void OnSceneChanged();

    void MoveRow(int delta);
    void AddSeparator();
    void RestoreDefaults();

    void AddActionFromProgram();
    void AddActionFromPreset(int presetIndex);
    void RemoveAction();
    void MoveAction(int delta);
    void ReloadActionList();

    void Save();
    void Cancel();

    std::wstring SceneTitle(MenuScene scene) const;
    MenuScene SelectedScene() const;
    std::wstring SceneKeyOf(MenuScene scene) const;

    HWND m_hOwner = nullptr;

    HWND m_hSceneLabel = nullptr;
    HWND m_hSceneCombo = nullptr;
    HWND m_hList = nullptr;
    HWND m_hActionLabel = nullptr;
    HWND m_hActionList = nullptr;
    HWND m_hUp = nullptr;
    HWND m_hDown = nullptr;
    HWND m_hAddSep = nullptr;
    HWND m_hRestore = nullptr;
    HWND m_hAddProgram = nullptr;
    HWND m_hAddPreset = nullptr;
    HWND m_hRemoveAction = nullptr;
    HWND m_hActionUp = nullptr;
    HWND m_hActionDown = nullptr;
    HWND m_hSave = nullptr;
    HWND m_hCancel = nullptr;

    HFONT m_font = nullptr;

    MenuLayout m_layout;
    MenuScene  m_scene = MenuScene::File;
    std::vector<Row> m_rows;

    bool m_saved = false;
};

} // namespace ui
