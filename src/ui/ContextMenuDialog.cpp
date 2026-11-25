// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/ContextMenuDialog.cpp
//
// 「自定义右键菜单」配置窗口的实现。界面说明见 ContextMenuDialog.h。
//
// 这一版只做两件事：
//   1. 当前场景显示哪些内置项、按什么顺序（勾选 + 上下移 + 分隔线）；
//   2. 用户自己的快捷动作（选程序 / 预设，自动填参数，不需要写命令行）。
//
// 明确不做：不采集系统右键菜单、不调第三方 Shell 扩展、不做「系统菜单」入口。
// ============================================================================

#include "ui/ContextMenuDialog.h"

#include "core/Logger.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace ui
{

namespace
{

const wchar_t* const kClassName = L"FileExplorerContextMenuDialog";

const int kIdSceneCombo   = 3001;
const int kIdList         = 3002;
const int kIdUp           = 3003;
const int kIdDown         = 3004;
const int kIdAddSep       = 3005;
const int kIdRestore      = 3006;
const int kIdAddProgram   = 3007;
const int kIdAddPreset    = 3008;
const int kIdRemoveAction = 3009;
const int kIdActionUp     = 3010;
const int kIdActionDown   = 3011;
const int kIdSave         = 3012;
const int kIdCancel       = 3013;
const int kIdActionList   = 3014;

const int kDialogWidth  = 820;
const int kDialogHeight = 660;

const COLORREF kHintColor = RGB(96, 103, 111);

std::wstring FileStem(const std::wstring& path)
{
    const wchar_t* name = ::PathFindFileNameW(path.c_str());
    std::wstring stem = (name != nullptr) ? name : path;
    const size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0)
    {
        stem = stem.substr(0, dot);
    }
    return stem;
}

bool FileExists(const std::wstring& path)
{
    if (path.empty()) return false;
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring EnvValue(const wchar_t* name)
{
    wchar_t buffer[32768] = {};
    const DWORD length = ::GetEnvironmentVariableW(name, buffer, 32768);
    return length > 0 ? std::wstring(buffer, length) : std::wstring();
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

// 预设：只放本机常见、能用一条命令表达的动作。
// 取不到的预设不在菜单里出现，不猜、不硬编码用户目录。
struct PresetInfo
{
    const wchar_t* title;
    const wchar_t* arguments;
    unsigned       scenes;
    const wchar_t* extensions;   // 分号分隔；空 = 全部
};

const PresetInfo kPresets[] =
{
    { L"用 VS Code 打开",            L"%1",                                            0x0E, L"" },
    { L"7-Zip 解压到当前文件夹",     L"x %1 -o%dir%",                                  0x02, L".zip;.7z;.rar;.tar;.gz;.bz2;.xz;.iso" },
    { L"WinRAR 解压到当前文件夹",    L"x -y %1 %dir%",                                0x02, L".zip;.7z;.rar;.tar;.gz;.bz2;.xz;.iso" },
    { L"在此打开 PowerShell",        L"-NoExit -Command Set-Location -LiteralPath %dir%", 0x05, L"" },
    { L"用记事本打开",               L"%1",                                            0x0A, L"" },
};

std::wstring PresetProgram(int index)
{
    const std::wstring programFiles = EnvValue(L"ProgramFiles");
    const std::wstring programFiles64 = EnvValue(L"ProgramW6432");
    const std::wstring localAppData = EnvValue(L"LOCALAPPDATA");
    const std::wstring systemRoot = EnvValue(L"SystemRoot");

    switch (index)
    {
    case 0:   // VS Code
    {
        const wchar_t* candidates[] =
        {
            L"Programs\\Microsoft VS Code\\Code.exe",
        };
        for (const wchar_t* relative : candidates)
        {
            const std::wstring path = JoinPath(localAppData, relative);
            if (FileExists(path)) return path;
        }
        const std::wstring path = JoinPath(programFiles, L"Microsoft VS Code\\Code.exe");
        return FileExists(path) ? path : std::wstring();
    }

    case 1:   // 7-Zip
    {
        for (const std::wstring& root : { programFiles, programFiles64 })
        {
            const std::wstring path = JoinPath(root, L"7-Zip\\7zG.exe");
            if (FileExists(path)) return path;
        }
        return std::wstring();
    }

    case 2:   // WinRAR
    {
        for (const std::wstring& root : { programFiles, programFiles64 })
        {
            const std::wstring path = JoinPath(root, L"WinRAR\\WinRAR.exe");
            if (FileExists(path)) return path;
        }
        return std::wstring();
    }

    case 3:   // PowerShell
    {
        const std::wstring path = JoinPath(systemRoot, L"System32\\WindowsPowerShell\\v1.0\\powershell.exe");
        return FileExists(path) ? path : std::wstring();
    }

    case 4:   // 记事本
    {
        const std::wstring path = JoinPath(systemRoot, L"System32\\notepad.exe");
        return FileExists(path) ? path : std::wstring();
    }

    default:
        return std::wstring();
    }
}

} // namespace

// ============================================================================
// 构造 / 析构
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

// ============================================================================
// 窗口类 / 显示
// ============================================================================

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

    if (hWnd == nullptr)
    {
        return false;
    }

    CreateControls();
    Layout();
    return true;
}

bool ContextMenuDialog::ShowModal()
{
    if (!EnsureClassRegistered())
    {
        return false;
    }

    LoadMenuLayout(m_layout);

    m_scene = MenuScene::File;
    m_saved = false;

    if (!CreateDialogWindow())
    {
        return false;
    }

    ReloadScene();
    ::SetFocus(m_hList);

    RunModalLoop(m_hOwner, m_hList);
    return m_saved;
}

// ============================================================================
// 创建控件
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
            0, 0, 10, 10, m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            instance, nullptr);
        if (control != nullptr && m_font != nullptr)
        {
            ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        }
        return control;
    };

    m_hSceneLabel = ::CreateWindowExW(
        0, L"STATIC", L"场景：",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
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

    m_hActionLabel = ::CreateWindowExW(
        0, L"STATIC", L"快捷动作（可选）：",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 10, 10, m_hWnd, nullptr, instance, nullptr);

    m_hActionList = ::CreateWindowExW(
        WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        0, 0, 10, 10, m_hWnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdActionList)), instance, nullptr);

    m_hUp          = createButton(L"上移", kIdUp);
    m_hDown        = createButton(L"下移", kIdDown);
    m_hAddSep      = createButton(L"加分隔线", kIdAddSep);
    m_hRestore     = createButton(L"恢复默认", kIdRestore);

    m_hAddProgram  = createButton(L"添加程序…", kIdAddProgram);
    m_hAddPreset   = createButton(L"预设 ▾", kIdAddPreset);
    m_hRemoveAction = createButton(L"删除", kIdRemoveAction);
    m_hActionUp    = createButton(L"上移", kIdActionUp);
    m_hActionDown  = createButton(L"下移", kIdActionDown);

    m_hSave        = createButton(L"保存", kIdSave);
    m_hCancel      = createButton(L"取消", kIdCancel);

    // 列表列
    LVCOLUMNW column = {};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(L"菜单项");
    column.cx = Scale(300);
    column.iSubItem = 0;
    ::SendMessageW(m_hList, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));

    column.pszText = const_cast<wchar_t*>(L"说明");
    column.cx = Scale(160);
    column.iSubItem = 1;
    ::SendMessageW(m_hList, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));

    ::SendMessageW(m_hList, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                   LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    // 场景下拉
    for (MenuScene scene : { MenuScene::Background, MenuScene::File,
                             MenuScene::Folder, MenuScene::Multiple })
    {
        ::SendMessageW(m_hSceneCombo, CB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(SceneTitle(scene).c_str()));
    }
    ::SendMessageW(m_hSceneCombo, CB_SETCURSEL, 1, 0);

    for (HWND control : { m_hSceneLabel, m_hSceneCombo, m_hList, m_hActionLabel,
                          m_hActionList, m_hSave, m_hCancel })
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
    const int comboHeight = Scale(26);
    const int buttonHeight = Scale(30);
    const int lineGap = Scale(8);
    const int listTop = Scale(96);
    const int listHeight = std::max(Scale(160),
                                    static_cast<int>(client.bottom) - listTop - Scale(150));

    const int contentWidth = static_cast<int>(client.right) - padding * 2;
    const int leftWidth = contentWidth * 62 / 100;
    const int rightX = padding + leftWidth + padding;
    const int rightWidth = contentWidth - leftWidth - padding;

    ::MoveWindow(m_hSceneLabel, padding, Scale(54), Scale(56), comboHeight, TRUE);
    ::MoveWindow(m_hSceneCombo, padding + Scale(58), Scale(50),
                 Scale(240), Scale(200), TRUE);

    ::MoveWindow(m_hList, padding, listTop, leftWidth, listHeight, TRUE);
    ::MoveWindow(m_hActionLabel, rightX, Scale(54), rightWidth, comboHeight, TRUE);
    ::MoveWindow(m_hActionList, rightX, listTop, rightWidth, listHeight, TRUE);

    const int leftButtonY = listTop + listHeight + lineGap;
    int x = padding;
    ::MoveWindow(m_hUp, x, leftButtonY, Scale(64), buttonHeight, TRUE); x += Scale(72);
    ::MoveWindow(m_hDown, x, leftButtonY, Scale(64), buttonHeight, TRUE); x += Scale(72);
    ::MoveWindow(m_hAddSep, x, leftButtonY, Scale(96), buttonHeight, TRUE); x += Scale(104);
    ::MoveWindow(m_hRestore, x, leftButtonY, Scale(96), buttonHeight, TRUE);

    int rightButtonY = leftButtonY;
    ::MoveWindow(m_hAddProgram, rightX, rightButtonY, Scale(112), buttonHeight, TRUE);
    ::MoveWindow(m_hAddPreset, rightX + Scale(120), rightButtonY, Scale(92), buttonHeight, TRUE);
    ::MoveWindow(m_hRemoveAction, rightX + Scale(220), rightButtonY, Scale(64), buttonHeight, TRUE);

    rightButtonY += buttonHeight + Scale(6);
    ::MoveWindow(m_hActionUp, rightX, rightButtonY, Scale(64), buttonHeight, TRUE);
    ::MoveWindow(m_hActionDown, rightX + Scale(72), rightButtonY, Scale(64), buttonHeight, TRUE);

    const int bottomY = static_cast<int>(client.bottom) - padding - buttonHeight;
    ::MoveWindow(m_hSave, client.right - padding - Scale(88), bottomY,
                 Scale(88), buttonHeight, TRUE);
    ::MoveWindow(m_hCancel, client.right - padding - Scale(88) - Scale(96), bottomY,
                 Scale(88), buttonHeight, TRUE);
}

// ============================================================================
// 绘制提示
// ============================================================================

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
                L"「快捷动作」是用户自己的程序/预设，不采集第三方右键菜单。",
                -1, &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    ::SelectObject(dc, oldFont);
}

// ============================================================================
// 场景 ↔ 列表
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

void ContextMenuDialog::ReloadScene()
{
    m_rows.clear();

    const std::wstring key = SceneKeyOf(m_scene);
    auto it = m_layout.scenes.find(key);
    const std::vector<std::wstring> order =
        (it != m_layout.scenes.end()) ? it->second : DefaultMenuOrder(m_scene);

    std::vector<std::wstring> seen;

    for (const std::wstring& token : order)
    {
        if (token == L"sep")
        {
            Row row;
            row.kind = Row::Kind::Separator;
            row.label = L"──────── 分隔线 ────────";
            row.checked = true;
            m_rows.push_back(std::move(row));
            continue;
        }

        if (token == L"actions")
        {
            Row row;
            row.kind = Row::Kind::Actions;
            row.label = L"快捷动作（整组）";
            row.checked = true;
            m_rows.push_back(std::move(row));
            seen.push_back(token);
            continue;
        }

        for (const MenuItemInfo& info : MenuItemCatalog())
        {
            if (info.id != token) continue;

            Row row;
            row.kind = Row::Kind::Item;
            row.id = info.id;
            row.label = info.label;
            row.checked = true;
            m_rows.push_back(std::move(row));
            seen.push_back(token);
            break;
        }
    }

    // 清单里没有的项也列出来（不勾选），用户才能重新打开
    const unsigned sceneBit = 1u << static_cast<unsigned>(m_scene);
    for (const MenuItemInfo& info : MenuItemCatalog())
    {
        if ((info.scenes & sceneBit) == 0) continue;
        if (std::find(seen.begin(), seen.end(), info.id) != seen.end()) continue;

        Row row;
        row.kind = (info.id == L"actions") ? Row::Kind::Actions : Row::Kind::Item;
        row.id = info.id;
        row.label = info.label;
        row.checked = false;
        m_rows.push_back(std::move(row));
    }

    ReloadList();
}

void ContextMenuDialog::ReloadList()
{
    ::SendMessageW(m_hList, LVM_DELETEALLITEMS, 0, 0);

    for (size_t i = 0; i < m_rows.size(); ++i)
    {
        const Row& row = m_rows[i];

        std::wstring second;
        switch (row.kind)
        {
        case Row::Kind::Separator: second = L"分隔线"; break;
        case Row::Kind::Actions:   second = L"动作组"; break;
        case Row::Kind::Item:      second = row.id;   break;
        }

        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(row.label.c_str());
        const int index = static_cast<int>(::SendMessageW(
            m_hList, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)));

        if (index < 0) continue;

        LVITEMW sub = {};
        sub.iSubItem = 1;
        sub.pszText = const_cast<wchar_t*>(second.c_str());
        ::SendMessageW(m_hList, LVM_SETITEMTEXTW, index, reinterpret_cast<LPARAM>(&sub));

        ListView_SetCheckState(m_hList, index, row.checked ? TRUE : FALSE);
    }

    if (!m_rows.empty())
    {
        ListView_SetItemState(m_hList, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(m_hList, 0, FALSE);
    }

    ReloadActionList();
}

void ContextMenuDialog::CommitList()
{
    std::vector<std::wstring> order;

    for (size_t i = 0; i < m_rows.size(); ++i)
    {
        if (!ListView_GetCheckState(m_hList, static_cast<int>(i)))
        {
            continue;
        }

        const Row& row = m_rows[i];
        if (row.kind == Row::Kind::Separator)
        {
            if (!order.empty() && order.back() != L"sep")
            {
                order.push_back(L"sep");
            }
            continue;
        }

        if (row.kind == Row::Kind::Actions)
        {
            if (std::find(order.begin(), order.end(), L"actions") == order.end())
            {
                order.push_back(L"actions");
            }
            continue;
        }

        order.push_back(row.id);
    }

    while (!order.empty() && order.back() == L"sep")
    {
        order.pop_back();
    }

    m_layout.scenes[SceneKeyOf(m_scene)] = std::move(order);
}

void ContextMenuDialog::OnSceneChanged()
{
    CommitList();
    m_scene = SelectedScene();
    ReloadScene();
}

void ContextMenuDialog::MoveRow(int delta)
{
    if (m_rows.empty()) return;

    const int selected = ListView_GetNextItem(m_hList, -1, LVNI_SELECTED);
    if (selected < 0) return;

    const int target = selected + delta;
    if (target < 0 || target >= static_cast<int>(m_rows.size())) return;

    std::swap(m_rows[selected], m_rows[target]);
    ReloadList();
    ListView_SetItemState(m_hList, target, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(m_hList, target, FALSE);
}

void ContextMenuDialog::AddSeparator()
{
    int selected = ListView_GetNextItem(m_hList, -1, LVNI_SELECTED);
    if (selected < 0) selected = static_cast<int>(m_rows.size()) - 1;
    const int insertAt = std::max(0, selected);

    Row row;
    row.kind = Row::Kind::Separator;
    row.label = L"──────── 分隔线 ────────";
    row.checked = true;
    m_rows.insert(m_rows.begin() + insertAt, std::move(row));

    ReloadList();
    ListView_SetItemState(m_hList, insertAt, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(m_hList, insertAt, FALSE);
}

void ContextMenuDialog::RestoreDefaults()
{
    m_layout.scenes[SceneKeyOf(m_scene)] = DefaultMenuOrder(m_scene);
    ReloadScene();
}

// ============================================================================
// 快捷动作
// ============================================================================

namespace
{

int NextActionOrder(const std::vector<CustomAction>& actions)
{
    int maxOrder = 0;
    for (const CustomAction& action : actions)
    {
        maxOrder = std::max(maxOrder, action.order);
    }
    return maxOrder + 10;
}

std::vector<std::wstring> SplitExtensions(const wchar_t* text)
{
    std::vector<std::wstring> result;
    if (text == nullptr || *text == L'\0') return result;

    std::wstring current;
    for (const wchar_t* p = text; ; ++p)
    {
        if (*p == L';' || *p == L'\0')
        {
            if (!current.empty()) result.push_back(current);
            current.clear();
            if (*p == L'\0') break;
            continue;
        }
        current.push_back(*p);
    }
    return result;
}

} // namespace

void ContextMenuDialog::ReloadActionList()
{
    ::SendMessageW(m_hActionList, LB_RESETCONTENT, 0, 0);

    for (const CustomAction& action : m_layout.actions)
    {
        const std::wstring text = action.title + L"    —    " + action.program;
        ::SendMessageW(m_hActionList, LB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(text.c_str()));
    }

    if (!m_layout.actions.empty())
    {
        ::SendMessageW(m_hActionList, LB_SETCURSEL, 0, 0);
    }
}

void ContextMenuDialog::AddActionFromProgram()
{
    wchar_t file[MAX_PATH] = {};

    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = m_hWnd;
    dialog.lpstrFilter = L"程序 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (!::GetOpenFileNameW(&dialog))
    {
        return;
    }

    const std::wstring program = file;
    if (!FileExists(program))
    {
        return;
    }

    CustomAction action;
    action.id        = NewActionId(m_layout);
    action.title     = L"用 " + FileStem(program) + L" 打开";
    action.program   = program;
    action.arguments = L"%1";
    action.startIn   = L"%dir%";
    action.scenes    = SceneBitFile | SceneBitFolder | SceneBitMultiple;
    action.order     = NextActionOrder(m_layout.actions);

    m_layout.actions.push_back(std::move(action));
    ReloadActionList();
    ::SendMessageW(m_hActionList, LB_SETCURSEL,
                   static_cast<WPARAM>(m_layout.actions.size() - 1), 0);
}

void ContextMenuDialog::AddActionFromPreset(int presetIndex)
{
    if (presetIndex < 0 || presetIndex >= static_cast<int>(std::size(kPresets)))
    {
        return;
    }

    const std::wstring program = PresetProgram(presetIndex);
    if (program.empty())
    {
        return;
    }

    const PresetInfo& preset = kPresets[presetIndex];

    CustomAction action;
    action.id          = NewActionId(m_layout);
    action.title       = preset.title;
    action.program     = program;
    action.arguments   = preset.arguments;
    action.startIn     = L"%dir%";
    action.scenes      = preset.scenes;
    action.extensions  = SplitExtensions(preset.extensions);
    action.order       = NextActionOrder(m_layout.actions);

    m_layout.actions.push_back(std::move(action));
    ReloadActionList();
    ::SendMessageW(m_hActionList, LB_SETCURSEL,
                   static_cast<WPARAM>(m_layout.actions.size() - 1), 0);
}

void ContextMenuDialog::RemoveAction()
{
    const int selected = static_cast<int>(::SendMessageW(m_hActionList, LB_GETCURSEL, 0, 0));
    if (selected < 0 || selected >= static_cast<int>(m_layout.actions.size()))
    {
        return;
    }

    m_layout.actions.erase(m_layout.actions.begin() + selected);
    ReloadActionList();
}

void ContextMenuDialog::MoveAction(int delta)
{
    const int selected = static_cast<int>(::SendMessageW(m_hActionList, LB_GETCURSEL, 0, 0));
    const int target = selected + delta;
    if (selected < 0 || target < 0 || target >= static_cast<int>(m_layout.actions.size()))
    {
        return;
    }

    std::swap(m_layout.actions[selected], m_layout.actions[target]);
    for (size_t i = 0; i < m_layout.actions.size(); ++i)
    {
        m_layout.actions[i].order = static_cast<int>(i) * 10;
    }

    ReloadActionList();
    ::SendMessageW(m_hActionList, LB_SETCURSEL, static_cast<WPARAM>(target), 0);
}

void ContextMenuDialog::Save()
{
    CommitList();

    // 四个场景全空 = 右键菜单彻底消失。这几乎一定是误操作，拦住。
    bool anyScene = false;
    for (const auto& entry : m_layout.scenes)
    {
        if (!entry.second.empty())
        {
            anyScene = true;
            break;
        }
    }
    if (!anyScene)
    {
        ::MessageBoxW(m_hWnd, L"四个场景的菜单项都被隐藏了，右键菜单会变成空白。\n"
                              L"请至少在一个场景里保留一项。",
                      L"自定义右键菜单", MB_OK | MB_ICONWARNING);
        return;
    }

    // 动作顺序按列表重排，保证 JSON 里的 order 和界面一致
    for (size_t i = 0; i < m_layout.actions.size(); ++i)
    {
        m_layout.actions[i].order = static_cast<int>(i) * 10;
    }

    if (!SaveMenuLayout(m_layout))
    {
        ::MessageBoxW(m_hWnd, L"保存失败：%APPDATA%\\FileExplorer\\context-menu.json 写不进去。\n"
                              L"请检查目录权限后重试。",
                      L"自定义右键菜单", MB_OK | MB_ICONERROR);
        return;
    }

    InvalidateMenuLayoutCache();
    Logger::Info(L"自定义右键菜单：已保存，动作 " +
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
        case kIdUp:            MoveRow(-1); break;
        case kIdDown:          MoveRow(1); break;
        case kIdAddSep:        AddSeparator(); break;
        case kIdRestore:       RestoreDefaults(); break;
        case kIdAddProgram:    AddActionFromProgram(); break;

        case kIdAddPreset:
        {
            HMENU menu = ::CreatePopupMenu();
            if (menu == nullptr) break;

            int count = 0;
            for (int i = 0; i < static_cast<int>(std::size(kPresets)); ++i)
            {
                if (PresetProgram(i).empty()) continue;
                ::AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(i + 1), kPresets[i].title);
                ++count;
            }

            if (count == 0)
            {
                ::DestroyMenu(menu);
                ::MessageBoxW(hWnd, L"没有检测到可用的预设程序。\n"
                                    L"可以点「添加程序…」自己选一个 exe。",
                              L"自定义右键菜单", MB_OK | MB_ICONINFORMATION);
                break;
            }

            POINT point = {};
            ::GetCursorPos(&point);
            const int command = static_cast<int>(::TrackPopupMenu(
                menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, hWnd, nullptr));
            ::DestroyMenu(menu);

            if (command > 0)
            {
                AddActionFromPreset(command - 1);
            }
            break;
        }

        case kIdRemoveAction:  RemoveAction(); break;
        case kIdActionUp:      MoveAction(-1); break;
        case kIdActionDown:    MoveAction(1); break;
        case kIdSave:          Save(); break;
        case kIdCancel:        Cancel(); break;

        default:
            break;
        }
        return 0;
    }

    case WM_KEYDOWN:
    {
        if (wParam == VK_ESCAPE)
        {
            Cancel();
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        Cancel();
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace ui
