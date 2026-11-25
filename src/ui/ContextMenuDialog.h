// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/ContextMenuDialog.h
//
// 「自定义右键菜单」配置窗口（单列表版）。
//
//   ┌──────────────────────────────────────────────────────────────┐
//   │ 场景 [单选文件 ▾]   勾选要显示的项；列表顺序就是菜单顺序      │
//   ├──────────────────────────────────────────────────────────────┤
//   │ ☑ 打开                                                       │
//   │ ☑ 打开方式                                                   │
//   │ ...                                                          │
//   │ ☐ WPS Office（经典）                                         │
//   │ ☐ WPS Office（现代）                                         │
//   │ ☐ 7-Zip（经典）                                              │
//   │ ☐ 用 XXX 打开（手动）                                        │
//   │ ☑ 属性                                                       │
//   ├──────────────────────────────────────────────────────────────┤
//   │ [添加程序…] [上移] [下移] [删除] [恢复默认]    [保存] [取消] │
//   └──────────────────────────────────────────────────────────────┘
//
// 规则：
//   · 第三方按「应用 + 经典/现代」平铺；默认不勾；设置里不展开、不预览功能。
//   · 右键菜单里只显示应用名，不显示括号；勾了经典+现代就是两条同名子菜单。
//   · 「添加程序…」是手动加一条「用 XXX 打开」，不是第三方动态组。
//   · 「删除」只删手动程序；第三方靠取消勾选。
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

    bool ShowModal();

private:
    struct Row
    {
        enum class Kind { Builtin, Separator, ThirdParty, Manual };

        Kind         kind = Kind::Builtin;
        std::wstring id;       // Builtin：内置 ID；Manual：动作 ID
        std::wstring key;      // ThirdParty：AppSourceKey
        std::wstring appName;  // ThirdParty
        int          source = 0;   // ThirdParty：0 经典 / 1 现代
        std::wstring label;
        std::wstring note;
        bool         checked = true;
    };

    LRESULT HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) override;

    static bool EnsureClassRegistered();
    bool CreateDialogWindow();
    void CreateControls();
    void Layout();
    void DrawHint(HDC dc);

    void ReloadScene();
    void ReloadList();
    void Commit();
    void OnSceneChanged();

    void MoveRow(int delta);
    void AddProgram();
    void DeleteRow();
    void RestoreDefaults();
    void Save();
    void Cancel();

    std::wstring SceneKeyOf(MenuScene scene) const;
    std::wstring SceneTitle(MenuScene scene) const;
    MenuScene SelectedScene() const;
    int SectionOf(const Row& row) const;

    HWND m_hOwner = nullptr;
    HWND m_hSceneLabel = nullptr;
    HWND m_hSceneCombo = nullptr;
    HWND m_hList = nullptr;
    HWND m_hAddProgram = nullptr;
    HWND m_hUp = nullptr;
    HWND m_hDown = nullptr;
    HWND m_hDelete = nullptr;
    HWND m_hRestore = nullptr;
    HWND m_hSave = nullptr;
    HWND m_hCancel = nullptr;
    HFONT m_font = nullptr;

    MenuLayout m_layout;
    MenuScene  m_scene = MenuScene::File;
    std::vector<Row> m_rows;
    bool m_saved = false;
};

} // namespace ui
