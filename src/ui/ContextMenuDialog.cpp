// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/ContextMenuDialog.cpp
//
// 单列表版「自定义右键菜单」配置窗口。说明见 ContextMenuDialog.h。
// ============================================================================

#include "ui/ContextMenuDialog.h"

#include "core/Logger.h"
#include "shell/MenuApps.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlwapi.h>

#include <algorithm>
#include <string>
#include <vector>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace ui
{

namespace
{

const wchar_t* const kClassName = L"FileExplorerContextMenuDialog";

const int kIdSceneCombo = 3001;
const int kIdList       = 3002;
const int kIdAddProgram = 3003;
const int kIdUp         = 3004;
const int kIdDown       = 3005;
const int kIdDelete     = 3006;
const int kIdRestore    = 3007;
const int kIdSave       = 3008;
const int kIdCancel     = 3009;

const int kDialogWidth  = 820;
const int kDialogHeight = 660;

const COLORREF kHintColor = RGB(96, 103, 111);

std::wstring FileStem(const std::wstring& path)
{
    const wchar_t* name = ::PathFindFileNameW(path.c_str());
    std::wstring stem = (name != nullptr) ? name : path;
    const size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) stem.resize(dot);
    return stem;
}

bool FileExists(const std::wstring& path)
{
    if (path.empty()) return false;
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

int NextActionOrder(const std::vector<CustomAction>& actions)
{
    int maxOrder = 0;
    for (const CustomAction& action : actions)
    {
        maxOrder = std::max(maxOrder, action.order);
    }
    return maxOrder + 10;
}

std::wstring SourceLabel(int source)
{
    return source == 1 ? L"现代" : L"经典";
}

} // namespace

// ============================================================================
// 构造 / 显示
// ============================================================================

ContextMenuDialog::ContextMenuDialog(HWND owner)
    : m_hOwner(owner)
{
    SetDpiSource(owner);
}

ContextMenuDialog::~ContextMenuDialog()
{
    if (m_font != nullptr)
    {
        ::DeleteObject(m_font);
        m_font = nullptr;
    }
}

bool ContextMenuDialog::EnsureClassRegistered()
{
    static bool registered = false;
    if (registered) return true;

    if (!RegisterWindowClass(kClassName, ::GetModuleHandleW(nullptr)))
    {
        return false;
    }
    registered = true;
    return true;
}

bool ContextMenuDialog::CreateDialogWindow()
{
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;

    RECT rect = { 0, 0, Scale(kDialogWidth), Scale(kDialogHeight) };
    ::AdjustWindowRectEx(&rect, style, FALSE, WS_EX_DLGMODALFRAME);

    HWND hWnd = ::CreateWindowExW(
        WS_EX_DLGMODALFRAME, kClassName, L"自定义右键菜单", style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        m_hOwner, nullptr, ::GetModuleHandleW(nullptr), this);
    if (hWnd == nullptr) return false;

    CreateControls();
    Layout();
    return true;
}

bool ContextMenuDialog::ShowModal()
{
    if (!EnsureClassRegistered()) return false;

    LoadMenuLayout(m_layout);
    m_scene = MenuScene::File;
    m_saved = false;

    if (!CreateDialogWindow()) return false;

    ReloadScene();
    ::SetFocus(m_hList);

    RunModalLoop(m_hOwner, m_hList);
    return m_saved;
}

// ============================================================================
// 控件
// ============================================================================

void ContextMenuDialog::CreateControls()
{
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);

    LOGFONTW lf = {};
    lf.lfHeight = -Scale(13);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, L"Microsoft YaHei UI");
    m_font = ::CreateFontIndirectW(&lf);

    auto createButton = [&](const wchar_t* text, int id)
    {
        HWND control = ::CreateWindowExW(
            0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 10, 10, m_hWnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        if (control != nullptr && m_font != nullptr)
        {
            ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        }
        return control;
    };

    m_hSceneLabel = ::CreateWindowExW(
        0, L"STATIC", L"场景：", WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 10, 10, m_hWnd, nullptr, instance, nullptr);

    m_hSceneCombo = ::CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 10, 200, m_hWnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSceneCombo)), instance, nullptr);

    m_hList = ::CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 10, 10, m_hWnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdList)), instance, nullptr);

    m_hAddProgram = createButton(L"添加程序…", kIdAddProgram);
    m_hUp         = createButton(L"上移", kIdUp);
    m_hDown       = createButton(L"下移", kIdDown);
    m_hDelete     = createButton(L"删除", kIdDelete);
    m_hRestore    = createButton(L"恢复默认", kIdRestore);
    m_hSave       = createButton(L"保存", kIdSave);
    m_hCancel     = createButton(L"取消", kIdCancel);

    LVCOLUMNW column = {};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(L"菜单项");
    column.cx = Scale(460);
    column.iSubItem = 0;
    ::SendMessageW(m_hList, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));

    column.pszText = const_cast<wchar_t*>(L"类型");
    column.cx = Scale(220);
    column.iSubItem = 1;
    ::SendMessageW(m_hList, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));

    ::SendMessageW(m_hList, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                   LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    for (MenuScene scene : { MenuScene::Background, MenuScene::File,
                             MenuScene::Folder, MenuScene::Multiple })
    {
        ::SendMessageW(m_hSceneCombo, CB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(SceneTitle(scene).c_str()));
    }
    ::SendMessageW(m_hSceneCombo, CB_SETCURSEL, 1, 0);

    for (HWND control : { m_hSceneLabel, m_hSceneCombo, m_hList,
                          m_hAddProgram, m_hUp, m_hDown, m_hDelete, m_hRestore,
                          m_hSave, m_hCancel })
    {
        if (control != nullptr && m_font != nullptr)
        {
            ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        }
    }
}

void ContextMenuDialog::Layout()
{
    RECT client = {};
    ::GetClientRect(m_hWnd, &client);

    const int padding = Scale(16);
    const int controlHeight = Scale(26);
    const int buttonHeight = Scale(30);
    const int listTop = Scale(96);
    const int bottomY = static_cast<int>(client.bottom) - padding - buttonHeight;
    const int listHeight = std::max(Scale(180),
                                    bottomY - listTop - buttonHeight - Scale(16));

    const int width = static_cast<int>(client.right) - padding * 2;

    ::MoveWindow(m_hSceneLabel, padding, Scale(54), Scale(56), controlHeight, TRUE);
    ::MoveWindow(m_hSceneCombo, padding + Scale(58), Scale(50),
                 Scale(240), Scale(200), TRUE);
    ::MoveWindow(m_hList, padding, listTop, width, listHeight, TRUE);

    int x = padding;
    ::MoveWindow(m_hAddProgram, x, listTop + listHeight + Scale(10),
                 Scale(116), buttonHeight, TRUE);
    x += Scale(124);
    ::MoveWindow(m_hUp, x, listTop + listHeight + Scale(10),
                 Scale(64), buttonHeight, TRUE);
    x += Scale(72);
    ::MoveWindow(m_hDown, x, listTop + listHeight + Scale(10),
                 Scale(64), buttonHeight, TRUE);
    x += Scale(72);
    ::MoveWindow(m_hDelete, x, listTop + listHeight + Scale(10),
                 Scale(64), buttonHeight, TRUE);
    x += Scale(80);
    ::MoveWindow(m_hRestore, x, listTop + listHeight + Scale(10),
                 Scale(96), buttonHeight, TRUE);

    ::MoveWindow(m_hSave, static_cast<int>(client.right) - padding - Scale(88), bottomY,
                 Scale(88), buttonHeight, TRUE);
    ::MoveWindow(m_hCancel,
                 static_cast<int>(client.right) - padding - Scale(88) - Scale(96), bottomY,
                 Scale(88), buttonHeight, TRUE);
}

void ContextMenuDialog::DrawHint(HDC dc)
{
    RECT client = {};
    ::GetClientRect(m_hWnd, &client);

    RECT hint = { Scale(16), Scale(12), client.right - Scale(16), Scale(44) };
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, kHintColor);
    HGDIOBJ oldFont = ::SelectObject(dc, m_font);
    ::DrawTextW(dc,
                L"勾选要显示的项；列表顺序就是右键菜单里的顺序。"
                L"第三方按应用+经典/现代平铺，默认不勾；括号只在设置里显示。",
                -1, &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    ::SelectObject(dc, oldFont);
}

// ============================================================================
// 场景 → 行
// ============================================================================

std::wstring ContextMenuDialog::SceneKeyOf(MenuScene scene) const
{
    return MenuSceneKey(scene);
}

std::wstring ContextMenuDialog::SceneTitle(MenuScene scene) const
{
    switch (scene)
    {
    case MenuScene::Background: return L"文件夹空白处";
    case MenuScene::File:       return L"单选文件";
    case MenuScene::Folder:     return L"单选文件夹";
    case MenuScene::Multiple:   return L"多选";
    }
    return L"单选文件";
}

MenuScene ContextMenuDialog::SelectedScene() const
{
    const int index = static_cast<int>(::SendMessageW(m_hSceneCombo, CB_GETCURSEL, 0, 0));
    switch (index)
    {
    case 0: return MenuScene::Background;
    case 2: return MenuScene::Folder;
    case 3: return MenuScene::Multiple;
    default: return MenuScene::File;
    }
}

int ContextMenuDialog::SectionOf(const Row& row) const
{
    switch (row.kind)
    {
    case Row::Kind::Builtin:
    case Row::Kind::Separator:  return 0;
    case Row::Kind::ThirdParty: return 1;
    case Row::Kind::Manual:     return 2;
    }
    return 0;
}

void ContextMenuDialog::ReloadScene()
{
    m_rows.clear();

    const std::wstring sceneKey = SceneKeyOf(m_scene);
    const unsigned sceneBit = 1u << static_cast<unsigned>(m_scene);

    auto orderIt = m_layout.scenes.find(sceneKey);
    const std::vector<std::wstring> order =
        (orderIt != m_layout.scenes.end()) ? orderIt->second : DefaultMenuOrder(m_scene);

    // ---- 第三方行：已勾的按配置顺序在前，其余按应用名在后 ----
    std::vector<Row> thirdRows;
    std::vector<ThirdPartySelection> selections;
    auto selectionIt = m_layout.thirdParty.find(sceneKey);
    if (selectionIt != m_layout.thirdParty.end())
    {
        selections = selectionIt->second;
    }

    std::vector<std::wstring> usedKeys;
    for (const ThirdPartySelection& selection : selections)
    {
        const shell::AppSourceRow* app = shell::FindAppSourceRow(selection.key);
        if (app == nullptr) continue;
        if ((app->scenes & sceneBit) == 0) continue;

        Row row;
        row.kind = Row::Kind::ThirdParty;
        row.key = selection.key;
        row.appName = selection.appName.empty() ? app->appName : selection.appName;
        row.source = selection.source;
        row.label = row.appName + (row.source == 1 ? L"（现代）" : L"（经典）");
        row.note = L"第三方 · " + SourceLabel(row.source);
        row.checked = true;
        thirdRows.push_back(std::move(row));
        usedKeys.push_back(selection.key);
    }

    for (const shell::AppSourceRow& app : shell::CachedAppSourceRows())
    {
        if ((app.scenes & sceneBit) == 0) continue;
        if (std::find(usedKeys.begin(), usedKeys.end(), app.key) != usedKeys.end()) continue;

        Row row;
        row.kind = Row::Kind::ThirdParty;
        row.key = app.key;
        row.appName = app.appName;
        row.source = static_cast<int>(app.source);
        row.label = app.appName + (row.source == 1 ? L"（现代）" : L"（经典）");
        row.note = L"第三方 · " + SourceLabel(row.source);
        row.checked = false;
        thirdRows.push_back(std::move(row));
    }

    // ---- 手动程序行 ----
    std::vector<Row> manualRows;
    for (const CustomAction& action : m_layout.actions)
    {
        Row row;
        row.kind = Row::Kind::Manual;
        row.id = action.id;
        row.label = action.title.empty() ? L"（未命名程序）" : action.title;
        row.note = L"手动程序";
        row.checked = action.enabled;
        manualRows.push_back(std::move(row));
    }

    // ---- 内置行 + 在 properties 前插入第三/手动块 ----
    bool blockInserted = false;
    auto insertBlock = [&]()
    {
        if (blockInserted) return;
        blockInserted = true;
        for (Row& row : thirdRows) m_rows.push_back(std::move(row));
        for (Row& row : manualRows) m_rows.push_back(std::move(row));
    };

    for (const std::wstring& token : order)
    {
        if (token == L"actions")
        {
            insertBlock();
            continue;
        }

        if (token == L"sep")
        {
            Row row;
            row.kind = Row::Kind::Separator;
            row.label = L"──────── 分隔线 ────────";
            row.note = L"分隔线";
            row.checked = true;
            m_rows.push_back(std::move(row));
            continue;
        }

        if (token == L"properties")
        {
            insertBlock();
        }

        const MenuItemInfo* info = nullptr;
        for (const MenuItemInfo& candidate : MenuItemCatalog())
        {
            if (candidate.id == token)
            {
                info = &candidate;
                break;
            }
        }
        if (info == nullptr) continue;

        Row row;
        row.kind = Row::Kind::Builtin;
        row.id = info->id;
        row.label = info->label;
        row.note = L"内置项";
        row.checked = true;
        m_rows.push_back(std::move(row));
    }

    insertBlock();
    ReloadList();
}

void ContextMenuDialog::ReloadList()
{
    ::SendMessageW(m_hList, LVM_DELETEALLITEMS, 0, 0);

    for (size_t i = 0; i < m_rows.size(); ++i)
    {
        const Row& row = m_rows[i];

        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(row.label.c_str());
        const int index = static_cast<int>(::SendMessageW(
            m_hList, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)));
        if (index < 0) continue;

        LVITEMW sub = {};
        sub.iSubItem = 1;
        sub.pszText = const_cast<wchar_t*>(row.note.c_str());
        ::SendMessageW(m_hList, LVM_SETITEMTEXTW, index, reinterpret_cast<LPARAM>(&sub));

        ListView_SetCheckState(m_hList, index, row.checked ? TRUE : FALSE);
    }

    if (!m_rows.empty())
    {
        ListView_SetItemState(m_hList, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(m_hList, 0, FALSE);
    }
}

void ContextMenuDialog::Commit()
{
    const std::wstring sceneKey = SceneKeyOf(m_scene);

    std::vector<std::wstring> builtins;
    std::vector<ThirdPartySelection> third;
    std::vector<CustomAction> actions;
    bool needActionsToken = false;

    for (size_t i = 0; i < m_rows.size(); ++i)
    {
        const Row& row = m_rows[i];
        const bool checked = ListView_GetCheckState(m_hList, static_cast<int>(i)) != FALSE;

        switch (row.kind)
        {
        case Row::Kind::Separator:
            if (checked && !builtins.empty() && builtins.back() != L"sep")
            {
                builtins.push_back(L"sep");
            }
            break;

        case Row::Kind::Builtin:
            if (checked) builtins.push_back(row.id);
            break;

        case Row::Kind::ThirdParty:
            if (checked)
            {
                ThirdPartySelection selection;
                selection.key = row.key;
                selection.appName = row.appName;
                selection.source = row.source;
                third.push_back(std::move(selection));
                needActionsToken = true;
            }
            break;

        case Row::Kind::Manual:
        {
            // 手动程序即使取消勾选也保留在配置里，只置 enabled=false
            for (const CustomAction& action : m_layout.actions)
            {
                if (action.id == row.id)
                {
                    CustomAction copy = action;
                    copy.enabled = checked;
                    copy.order = static_cast<int>(actions.size()) * 10;
                    actions.push_back(std::move(copy));
                    if (checked) needActionsToken = true;
                    break;
                }
            }
            break;
        }
        }
    }

    while (!builtins.empty() && builtins.back() == L"sep")
    {
        builtins.pop_back();
    }

    // 第三/手动的插入点：在内置属性之前；没有属性就放末尾
    const size_t actionsPos = std::find(builtins.begin(), builtins.end(), L"actions") - builtins.begin();
    if (actionsPos < builtins.size())
    {
        builtins.erase(builtins.begin() + actionsPos);
    }
    if (needActionsToken)
    {
        auto properties = std::find(builtins.begin(), builtins.end(), L"properties");
        builtins.insert(properties, L"actions");
    }

    m_layout.scenes[sceneKey] = std::move(builtins);
    m_layout.thirdParty[sceneKey] = std::move(third);
    m_layout.actions = std::move(actions);
}

void ContextMenuDialog::OnSceneChanged()
{
    Commit();
    m_scene = SelectedScene();
    ReloadScene();
}

// ============================================================================
// 按钮
// ============================================================================

void ContextMenuDialog::MoveRow(int delta)
{
    if (m_rows.empty()) return;

    const int selected = ListView_GetNextItem(m_hList, -1, LVNI_SELECTED);
    if (selected < 0) return;

    const int target = selected + delta;
    if (target < 0 || target >= static_cast<int>(m_rows.size())) return;

    if (SectionOf(m_rows[selected]) != SectionOf(m_rows[target]))
    {
        return;   // 内置 / 第三方 / 手动三块之间不混排
    }

    std::swap(m_rows[selected], m_rows[target]);
    ReloadList();
    ListView_SetItemState(m_hList, target, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(m_hList, target, FALSE);
}

void ContextMenuDialog::AddProgram()
{
    wchar_t file[MAX_PATH] = {};

    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = m_hWnd;
    dialog.lpstrFilter = L"程序 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (!::GetOpenFileNameW(&dialog)) return;

    const std::wstring program = file;
    if (!FileExists(program)) return;

    CustomAction action;
    action.id        = NewActionId(m_layout);
    action.title     = L"用 " + FileStem(program) + L" 打开";
    action.program   = program;
    action.arguments = L"%1";
    action.startIn   = L"%dir%";
    action.scenes    = SceneBitFile | SceneBitFolder | SceneBitMultiple;
    action.enabled   = true;
    action.order     = NextActionOrder(m_layout.actions);

    m_layout.actions.push_back(std::move(action));
    ReloadScene();
}

void ContextMenuDialog::DeleteRow()
{
    const int selected = ListView_GetNextItem(m_hList, -1, LVNI_SELECTED);
    if (selected < 0 || selected >= static_cast<int>(m_rows.size())) return;

    const Row& row = m_rows[selected];
    if (row.kind != Row::Kind::Manual)
    {
        // 第三方靠取消勾选；内置项不能删
        MessageBeep(MB_ICONINFORMATION);
        return;
    }

    const std::wstring id = row.id;
    m_layout.actions.erase(
        std::remove_if(m_layout.actions.begin(), m_layout.actions.end(),
                       [&id](const CustomAction& action) { return action.id == id; }),
        m_layout.actions.end());
    ReloadScene();
}

void ContextMenuDialog::RestoreDefaults()
{
    m_layout.scenes[SceneKeyOf(m_scene)] = DefaultMenuOrder(m_scene);
    m_layout.thirdParty[SceneKeyOf(m_scene)].clear();
    ReloadScene();
}

void ContextMenuDialog::Save()
{
    Commit();

    if (!SaveMenuLayout(m_layout))
    {
        ::MessageBoxW(m_hWnd, L"保存失败：%APPDATA%\\FileExplorer\\context-menu.json 写不进去。\n"
                              L"请检查目录权限后重试。",
                      L"自定义右键菜单", MB_OK | MB_ICONERROR);
        return;
    }

    InvalidateMenuLayoutCache();
    Logger::Info(L"自定义右键菜单：已保存，第三方 " +
                 std::to_wstring(m_layout.thirdParty[SceneKeyOf(m_scene)].size()) +
                 L" 条（当前场景），手动程序 " +
                 std::to_wstring(m_layout.actions.size()) + L" 个");

    m_saved = true;
    ::DestroyWindow(m_hWnd);
}

void ContextMenuDialog::Cancel()
{
    ::DestroyWindow(m_hWnd);
}

// ============================================================================
// 消息
// ============================================================================

LRESULT ContextMenuDialog::HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        RECT rect = {};
        ::GetClientRect(hWnd, &rect);
        ::FillRect(dc, &rect, reinterpret_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH)));
        return 1;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT paint = {};
        HDC dc = ::BeginPaint(hWnd, &paint);
        RECT rect = {};
        ::GetClientRect(hWnd, &rect);
        ::FillRect(dc, &rect, reinterpret_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH)));
        DrawHint(dc);
        ::EndPaint(hWnd, &paint);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, RGB(31, 35, 40));
        return reinterpret_cast<LRESULT>(::GetStockObject(WHITE_BRUSH));
    }

    case WM_COMMAND:
    {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);

        if (id == kIdSceneCombo && code == CBN_SELCHANGE)
        {
            OnSceneChanged();
            return 0;
        }

        if (code != BN_CLICKED) break;

        switch (id)
        {
        case kIdAddProgram: AddProgram(); break;
        case kIdUp:         MoveRow(-1); break;
        case kIdDown:       MoveRow(1); break;
        case kIdDelete:     DeleteRow(); break;
        case kIdRestore:    RestoreDefaults(); break;
        case kIdSave:       Save(); break;
        case kIdCancel:     Cancel(); break;
        default: break;
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            Cancel();
            return 0;
        }
        break;

    case WM_CLOSE:
        Cancel();
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace ui
