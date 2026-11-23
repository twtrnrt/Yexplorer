// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/ContextMenuSettingsDialog.cpp
//
// 「配置右键菜单」窗口（二期）实现。界面说明见同名 .h 文件。
//
// 数据链路：
//   打开窗口 → 读 menu-census.json（左侧树）+ menu-config.json（右侧树），
//   同时后台启动一次普查（独立子进程，界面不卡）；普查完再刷新左侧树。
//   右侧所有编辑动作即时写入 menu-config.json（用户拖进来的心血不会丢）。
//
// 本阶段只做配置：不写注册表、不接右键菜单弹出链路。
// ============================================================================

#include "ui/ContextMenuSettingsDialog.h"

#include "core/Logger.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <commdlg.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <stdio.h>
#include <functional>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

namespace ui
{

namespace
{

const wchar_t* const kClassName = L"FileExplorerContextMenuSettingsDialog";

// 控件 ID
const int kIdStatus     = 2201;
const int kIdRefresh    = 2202;
const int kIdSupplement = 2203;
const int kIdSave       = 2204;
const int kIdSysTree    = 2210;
const int kIdCfgTree    = 2211;
const int kIdPreview    = 2212;
const int kIdNameEdit   = 2220;
const int kIdIconBtn    = 2221;
const int kIdIconClear  = 2222;
const int kIdNewGroup   = 2223;
const int kIdDelete     = 2224;
const int kIdMoveUp     = 2225;
const int kIdMoveDown   = 2226;
const int kIdCheckFile  = 2227;
const int kIdCheckDir   = 2228;
const int kIdCheckBg    = 2229;
const int kIdCheckMulti = 2230;
const int kIdExtsEdit   = 2231;

// 窗口逻辑尺寸
const int kDialogWidth  = 1180;
const int kDialogHeight = 780;
const int kPadding      = 12;
const int kGap          = 10;
const int kButtonHeight = 28;
const int kRowHeight    = 34;   // 顶部按钮行高
const int kLabelHeight  = 18;
const int kPreviewRowH  = 24;
const int kIconPixels   = 18;

// 颜色
const COLORREF kTextColor        = RGB(31, 35, 40);
const COLORREF kMutedColor       = RGB(110, 118, 129);
const COLORREF kGroupTextColor   = RGB(9, 90, 160);
const COLORREF kFailColor        = RGB(178, 34, 34);

// 场景名（与 MenuScene 位对应）
std::wstring SceneName(unsigned int scene)
{
    switch (scene)
    {
    case shell::SceneFile:       return L"单选文件";
    case shell::SceneFolder:     return L"文件夹";
    case shell::SceneBackground: return L"文件夹空白区";
    case shell::SceneMultiple:   return L"多选文件";
    default:                     return L"其它";
    }
}

std::wstring Trim(const std::wstring& text)
{
    const size_t begin = text.find_first_not_of(L" \t\r\n");
    if (begin == std::wstring::npos) return std::wstring();
    const size_t end = text.find_last_not_of(L" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::wstring NowText()
{
    SYSTEMTIME st = {};
    ::GetLocalTime(&st);
    wchar_t buffer[64] = {};
    swprintf_s(buffer, L"%04d-%02d-%02d %02d:%02d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buffer;
}

// 显示用的条目文字：空文字给个占位，灰态标注出来
std::wstring ItemDisplayText(const shell::CensusItem& item)
{
    std::wstring text = item.text.empty() ? L"（无文字）" : item.text;
    if (item.disabled) text += L"（灰）";
    return text;
}

// 图标文件 → 完整路径（配置里既可能存文件名，也可能存用户选的绝对路径）
std::wstring ResolveIconPath(const std::wstring& iconFile)
{
    if (iconFile.empty()) return std::wstring();
    if (iconFile.find(L'\\') != std::wstring::npos ||
        iconFile.find(L'/') != std::wstring::npos ||
        iconFile.find(L':') != std::wstring::npos)
    {
        return iconFile;
    }
    const std::wstring dir = shell::IconDir();
    if (dir.empty()) return std::wstring();
    return dir + L"\\" + iconFile;
}

HBITMAP LoadBitmapFile(const std::wstring& iconFile, int pixels)
{
    const std::wstring path = ResolveIconPath(iconFile);
    if (path.empty()) return nullptr;
    return static_cast<HBITMAP>(::LoadImageW(nullptr, path.c_str(), IMAGE_BITMAP,
                                            pixels, pixels,
                                            LR_LOADFROMFILE | LR_CREATEDIBSECTION));
}

} // namespace

// ============================================================================
// 构造 / 析构
// ============================================================================

ContextMenuSettingsDialog::ContextMenuSettingsDialog(HWND owner)
{
    m_hOwner = owner;
    SetDpiSource(owner);
}

ContextMenuSettingsDialog::~ContextMenuSettingsDialog()
{
    // 普查线程若还在跑：置取消位后脱离（它只会 PostMessage 到已销毁的窗口，安全）
    m_censusCancel = true;
    if (m_censusThread.joinable())
    {
        m_censusThread.detach();
    }

    ClearIconList();
    ReleasePreviewBitmaps();

    if (m_font != nullptr)
    {
        ::DeleteObject(m_font);
        m_font = nullptr;
    }
}

// ============================================================================
// 窗口类 / 显示
// ============================================================================

bool ContextMenuSettingsDialog::EnsureClassRegistered()
{
    static bool registered = false;
    if (registered) return true;
    if (!RegisterWindowClass(kClassName, ::GetModuleHandleW(nullptr), CS_DBLCLKS))
    {
        return false;
    }
    registered = true;
    return true;
}

void ContextMenuSettingsDialog::ShowModal()
{
    if (!CreateDialogWindow())
    {
        return;
    }
    RunModalLoop(m_hOwner, m_hSysTree);
}

// ============================================================================
// 建窗
// ============================================================================

bool ContextMenuSettingsDialog::CreateDialogWindow()
{
    if (!EnsureClassRegistered()) return false;

    INITCOMMONCONTROLSEX cc = {};
    cc.dwSize = sizeof(cc);
    cc.dwICC  = ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES;
    ::InitCommonControlsEx(&cc);

    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
    {
        m_font = ::CreateFontIndirectW(&metrics.lfMessageFont);
    }
    if (m_font == nullptr)
    {
        m_font = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    }

    const int width  = Scale(kDialogWidth);
    const int height = Scale(kDialogHeight);

    RECT windowRect = { 0, 0, width, height };
    ::AdjustWindowRectEx(&windowRect, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE,
                         WS_EX_DLGMODALFRAME);
    const int windowW = windowRect.right - windowRect.left;
    const int windowH = windowRect.bottom - windowRect.top;

    RECT ownerRect = {};
    if (m_hOwner != nullptr) ::GetWindowRect(m_hOwner, &ownerRect);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - windowW) / 2;
    const int y = ownerRect.top  + ((ownerRect.bottom - ownerRect.top) - windowH) / 2;

    m_hWnd = ::CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                               kClassName,
                               L"配置右键菜单（二期：拖拽配置）",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               x, y, windowW, windowH,
                               m_hOwner, nullptr,
                               ::GetModuleHandleW(nullptr), this);
    if (m_hWnd == nullptr)
    {
        Logger::Error(L"配置右键菜单窗口创建失败");
        return false;
    }

    CreateControls();
    Layout();

    // 配置先读进来（很小），没有就当空配置
    ReloadConfig();
    RebuildCfgTree();
    RebuildPreview();

    // 普查缓存有就先显示，随后一定自动刷新一次（用户要求"打开就自动刷新"）
    m_hasReport = shell::LoadCensus(m_report);
    if (m_hasReport)
    {
        RebuildSysTree();
        RebuildCfgTree();      // 重建图标列表后，右侧树的图标也要重新挂上
        RebuildPreview();
        ::SetWindowTextW(m_hStatus,
                         (L"上次普查：" + m_report.scannedAt + L"　正在自动刷新…").c_str());
    }
    StartCensus();

    return true;
}

void ContextMenuSettingsDialog::CreateControls()
{
    auto create = [this](const wchar_t* cls, const wchar_t* text, DWORD style, int id) -> HWND
    {
        return ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                 0, 0, 0, 0, m_hWnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 ::GetModuleHandleW(nullptr), nullptr);
    };

    m_hStatus     = create(WC_STATICW, L"", SS_LEFT | SS_ENDELLIPSIS, kIdStatus);
    m_hRefresh    = create(WC_BUTTONW, L"刷新系统菜单", BS_PUSHBUTTON, kIdRefresh);
    m_hSupplement = create(WC_BUTTONW, L"一键补图标", BS_PUSHBUTTON, kIdSupplement);
    m_hSave       = create(WC_BUTTONW, L"保存", BS_PUSHBUTTON, kIdSave);

    m_labelSys   = create(WC_STATICW, L"系统右键菜单（只读，拖这里的项到右边）", SS_LEFT, 0);
    m_labelCfg   = create(WC_STATICW, L"我们的右键菜单（拖左侧项目进来）", SS_LEFT, 0);
    m_labelProps = create(WC_STATICW, L"选中项设置", SS_LEFT, 0);
    m_labelPreview = create(WC_STATICW, L"预览：我们的右键菜单弹出来是这样", SS_LEFT, 0);

    m_hSysTree = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                       TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
                                       TVS_SHOWSELALWAYS,
                                   0, 0, 0, 0, m_hWnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSysTree)),
                                   ::GetModuleHandleW(nullptr), nullptr);

    m_hCfgTree = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                       TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
                                       TVS_SHOWSELALWAYS,
                                   0, 0, 0, 0, m_hWnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCfgTree)),
                                   ::GetModuleHandleW(nullptr), nullptr);

    m_hNewGroup = create(WC_BUTTONW, L"新建组", BS_PUSHBUTTON, kIdNewGroup);
    m_hDelete   = create(WC_BUTTONW, L"删除", BS_PUSHBUTTON, kIdDelete);
    m_hMoveUp   = create(WC_BUTTONW, L"上移", BS_PUSHBUTTON, kIdMoveUp);
    m_hMoveDown = create(WC_BUTTONW, L"下移", BS_PUSHBUTTON, kIdMoveDown);

    m_hNameEdit  = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                     0, 0, 0, 0, m_hWnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdNameEdit)),
                                     ::GetModuleHandleW(nullptr), nullptr);
    m_hIconBtn   = create(WC_BUTTONW, L"换图标", BS_PUSHBUTTON, kIdIconBtn);
    m_hIconClear = create(WC_BUTTONW, L"清除图标", BS_PUSHBUTTON, kIdIconClear);
    m_hCheckFile = create(WC_BUTTONW, L"单选文件", BS_AUTOCHECKBOX, kIdCheckFile);
    m_hCheckDir  = create(WC_BUTTONW, L"文件夹", BS_AUTOCHECKBOX, kIdCheckDir);
    m_hCheckBg   = create(WC_BUTTONW, L"空白区", BS_AUTOCHECKBOX, kIdCheckBg);
    m_hCheckMulti = create(WC_BUTTONW, L"多选", BS_AUTOCHECKBOX, kIdCheckMulti);
    m_hExtsEdit  = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                     0, 0, 0, 0, m_hWnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdExtsEdit)),
                                     ::GetModuleHandleW(nullptr), nullptr);

    m_hPreview = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                       LVS_REPORT | LVS_SINGLESEL | LVS_NOCOLUMNHEADER |
                                       LVS_NOSORTHEADER | LVS_OWNERDRAWFIXED,
                                   0, 0, 0, 0, m_hWnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPreview)),
                                   ::GetModuleHandleW(nullptr), nullptr);
    ::SendMessageW(m_hPreview, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                   LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    {
        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<LPWSTR>(L"");
        column.cx = Scale(1200);
        ::SendMessageW(m_hPreview, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
    }

    HWND all[] = { m_hStatus, m_hRefresh, m_hSupplement, m_hSave,
                   m_labelSys, m_labelCfg, m_labelProps, m_labelPreview,
                   m_hSysTree, m_hCfgTree, m_hPreview,
                   m_hNameEdit, m_hIconBtn, m_hIconClear,
                   m_hNewGroup, m_hDelete, m_hMoveUp, m_hMoveDown,
                   m_hCheckFile, m_hCheckDir, m_hCheckBg, m_hCheckMulti, m_hExtsEdit };
    for (HWND control : all)
    {
        if (control != nullptr)
        {
            ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        }
    }

    ::SendMessageW(m_hSysTree, TVM_SETEXTENDEDSTYLE, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
    ::SendMessageW(m_hCfgTree, TVM_SETEXTENDEDSTYLE, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
}

void ContextMenuSettingsDialog::Layout()
{
    if (m_hWnd == nullptr) return;

    RECT client = {};
    ::GetClientRect(m_hWnd, &client);

    const int padding = Scale(kPadding);
    const int gap     = Scale(kGap);
    const int buttonH = Scale(kButtonHeight);
    const int rowH    = Scale(kRowHeight);
    const int labelH  = Scale(kLabelHeight);

    // 顶部：状态 + 三个按钮（右对齐）
    int right = client.right - padding;
    const int saveW = Scale(72);
    const int supW  = Scale(96);
    const int refW  = Scale(108);

    ::MoveWindow(m_hSave, right - saveW, padding, saveW, buttonH, TRUE); right -= saveW + gap;
    ::MoveWindow(m_hSupplement, right - supW, padding, supW, buttonH, TRUE); right -= supW + gap;
    ::MoveWindow(m_hRefresh, right - refW, padding, refW, buttonH, TRUE); right -= refW + gap;
    ::MoveWindow(m_hStatus, padding, padding + (buttonH - Scale(16)) / 2, right - padding, Scale(18), TRUE);

    const int contentTop = padding + rowH + Scale(4);
    const int contentBottom = client.bottom - padding;
    const int contentH = contentBottom - contentTop;

    // 左右两栏：左侧 44%，右侧剩余
    const int leftW  = (client.right - padding * 2 - gap) * 44 / 100;
    const int leftX  = padding;
    const int rightX = leftX + leftW + gap;
    const int rightW = client.right - padding - rightX;

    ::MoveWindow(m_labelSys, leftX, contentTop, leftW, labelH, TRUE);
    ::MoveWindow(m_hSysTree, leftX, contentTop + labelH + Scale(2), leftW,
                 contentH - labelH - Scale(2), TRUE);

    ::MoveWindow(m_labelCfg, rightX, contentTop, rightW, labelH, TRUE);

    // 右侧：配置树（约 34% 高度）+ 按钮行 + 属性区 + 预览
    const int cfgTop = contentTop + labelH + Scale(2);
    const int cfgH   = contentH * 34 / 100;
    ::MoveWindow(m_hCfgTree, rightX, cfgTop, rightW, cfgH, TRUE);

    const int btnTop = cfgTop + cfgH + Scale(4);
    const int smallW = Scale(72);
    ::MoveWindow(m_hNewGroup, rightX, btnTop, smallW, buttonH, TRUE);
    ::MoveWindow(m_hDelete, rightX + smallW + gap, btnTop, smallW, buttonH, TRUE);
    ::MoveWindow(m_hMoveUp, rightX + (smallW + gap) * 2, btnTop, smallW, buttonH, TRUE);
    ::MoveWindow(m_hMoveDown, rightX + (smallW + gap) * 3, btnTop, smallW, buttonH, TRUE);

    const int propsTop = btnTop + buttonH + Scale(6);
    ::MoveWindow(m_labelProps, rightX, propsTop, rightW, labelH, TRUE);

    const int nameTop = propsTop + labelH + Scale(2);
    const int nameLabelW = Scale(42);
    const int iconBtnW = Scale(72);
    ::MoveWindow(m_hNameEdit, rightX + nameLabelW, nameTop,
                 rightW - nameLabelW - iconBtnW * 2 - gap * 3, Scale(26), TRUE);
    ::MoveWindow(m_hIconBtn, rightX + rightW - iconBtnW * 2 - gap * 2, nameTop,
                 iconBtnW, Scale(26), TRUE);
    ::MoveWindow(m_hIconClear, rightX + rightW - iconBtnW - gap, nameTop,
                 iconBtnW, Scale(26), TRUE);

    const int scopeTop = nameTop + Scale(30);
    const int checkW = Scale(78);
    ::MoveWindow(m_hCheckFile, rightX, scopeTop, checkW, Scale(24), TRUE);
    ::MoveWindow(m_hCheckDir, rightX + checkW, scopeTop, checkW, Scale(24), TRUE);
    ::MoveWindow(m_hCheckBg, rightX + checkW * 2, scopeTop, checkW, Scale(24), TRUE);
    ::MoveWindow(m_hCheckMulti, rightX + checkW * 3, scopeTop, checkW, Scale(24), TRUE);

    const int extsTop = scopeTop + Scale(28);
    ::MoveWindow(m_hExtsEdit, rightX, extsTop, rightW, Scale(26), TRUE);

    const int previewTop = extsTop + Scale(32);
    ::MoveWindow(m_labelPreview, rightX, previewTop, rightW, labelH, TRUE);
    ::MoveWindow(m_hPreview, rightX, previewTop + labelH + Scale(2), rightW,
                 contentBottom - (previewTop + labelH + Scale(2)), TRUE);
}

// ============================================================================
// 图标列表
// ============================================================================

void ContextMenuSettingsDialog::ClearIconList()
{
    if (m_hSysTree != nullptr)
    {
        ::SendMessageW(m_hSysTree, TVM_SETIMAGELIST, TVSIL_NORMAL, 0);
    }
    if (m_hCfgTree != nullptr)
    {
        ::SendMessageW(m_hCfgTree, TVM_SETIMAGELIST, TVSIL_NORMAL, 0);
    }
    if (m_icons != nullptr)
    {
        ::ImageList_Destroy(m_icons);
        m_icons = nullptr;
    }
    m_iconFiles.clear();
}

int ContextMenuSettingsDialog::IconIndexFor(const std::wstring& iconFile)
{
    if (iconFile.empty()) return -1;

    // 已经加过的直接返回
    for (size_t i = 0; i < m_iconFiles.size(); ++i)
    {
        if (m_iconFiles[i] == iconFile) return static_cast<int>(i);
    }

    if (m_icons == nullptr)
    {
        const int pixels = Scale(kIconPixels);
        m_icons = ::ImageList_Create(pixels, pixels, ILC_COLOR32 | ILC_MASK, 8, 16);
        if (m_icons == nullptr) return -1;
        if (m_hSysTree != nullptr)
        {
            ::SendMessageW(m_hSysTree, TVM_SETIMAGELIST, TVSIL_NORMAL,
                           reinterpret_cast<LPARAM>(m_icons));
        }
        if (m_hCfgTree != nullptr)
        {
            ::SendMessageW(m_hCfgTree, TVM_SETIMAGELIST, TVSIL_NORMAL,
                           reinterpret_cast<LPARAM>(m_icons));
        }
    }

    HBITMAP bitmap = LoadBitmapFile(iconFile, Scale(kIconPixels));
    if (bitmap == nullptr) return -1;

    // 采集时图标都画在白底上，用白色当透明色正好能抠干净
    const int index = ::ImageList_AddMasked(m_icons, bitmap, RGB(255, 255, 255));
    ::DeleteObject(bitmap);
    if (index < 0) return -1;

    m_iconFiles.push_back(iconFile);
    return index;
}

// ============================================================================
// 左侧系统树
// ============================================================================

HTREEITEM ContextMenuSettingsDialog::AddTreeItem(HWND tree, HTREEITEM parent,
                                                 const std::wstring& text,
                                                 const std::wstring& iconFile,
                                                 LPARAM data)
{
    TVINSERTSTRUCTW insert = {};
    insert.hParent      = parent;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask    = TVIF_TEXT | TVIF_PARAM;
    insert.item.pszText = const_cast<LPWSTR>(text.c_str());
    insert.item.lParam  = data;

    const int iconIndex = IconIndexFor(iconFile);
    if (iconIndex >= 0)
    {
        insert.item.mask |= TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        insert.item.iImage = iconIndex;
        insert.item.iSelectedImage = iconIndex;
    }
    return reinterpret_cast<HTREEITEM>(
        ::SendMessageW(tree, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&insert)));
}

void ContextMenuSettingsDialog::RebuildSysTree()
{
    if (m_hSysTree == nullptr) return;

    ClearIconList();
    m_sysNodes.clear();
    ::SendMessageW(m_hSysTree, TVM_DELETEITEM, 0, reinterpret_cast<LPARAM>(TVI_ROOT));

    if (!m_hasReport)
    {
        return;
    }

    // 软件顺序：先按簇的品牌顺序（BuildClusters 已按重要度排好），
    // 再把只在非文件场景里出现的品牌补到后面。
    std::vector<std::wstring> brands;
    auto addBrand = [&brands](const std::wstring& brand)
    {
        for (const std::wstring& existing : brands)
        {
            if (existing == brand) return;
        }
        brands.push_back(brand);
    };

    for (const shell::CensusCluster& cluster : m_report.clusters) addBrand(cluster.brand);
    for (const shell::CensusScene& scene : m_report.scenes)
    {
        for (const shell::CensusItem& item : scene.items)
        {
            if (!item.separator) addBrand(item.brand.empty() ? L"其它" : item.brand);
        }
    }

    for (const std::wstring& brand : brands)
    {
        // 该品牌在本机"单选文件"场景下的簇
        std::vector<int> ownClusters;
        for (int i = 0; i < static_cast<int>(m_report.clusters.size()); ++i)
        {
            if (m_report.clusters[i].brand == brand) ownClusters.push_back(i);
        }

        // 该品牌在三个非文件场景里的条目
        std::vector<std::pair<unsigned int, std::vector<int>>> ownScenes;
        for (int s = 0; s < static_cast<int>(m_report.scenes.size()); ++s)
        {
            const shell::CensusScene& scene = m_report.scenes[s];
            std::vector<int> indexes;
            for (int i = 0; i < static_cast<int>(scene.items.size()); ++i)
            {
                const shell::CensusItem& item = scene.items[i];
                if (item.separator) continue;
                const std::wstring itemBrand = item.brand.empty() ? L"其它" : item.brand;
                if (itemBrand == brand) indexes.push_back(i);
            }
            if (!indexes.empty()) ownScenes.push_back({ scene.scene, indexes });
        }

        if (ownClusters.empty() && ownScenes.empty()) continue;

        SysNode software;
        software.kind  = SysNode::Software;
        software.brand = brand;
        m_sysNodes.push_back(software);
        const int softwareIndex = static_cast<int>(m_sysNodes.size()) - 1;

        std::wstring softwareText = brand;
        if (!ownClusters.empty())
        {
            softwareText += L"　（" + std::to_wstring(ownClusters.size()) + L" 簇）";
        }
        const HTREEITEM softwareItem = AddTreeItem(m_hSysTree, TVI_ROOT, softwareText,
                                                   shell::BrandIconFile(brand),
                                                   softwareIndex + 1);

        // ---- 单选文件场景：三级按扩展名分簇 ----
        if (!ownClusters.empty())
        {
            SysNode fileScene;
            fileScene.kind  = SysNode::Scene;
            fileScene.brand = brand;
            fileScene.scene = shell::SceneFile;
            m_sysNodes.push_back(fileScene);
            const int fileSceneIndex = static_cast<int>(m_sysNodes.size()) - 1;

            unsigned int extTotal = 0;
            for (int clusterIndex : ownClusters)
            {
                extTotal += static_cast<unsigned int>(m_report.clusters[clusterIndex].exts.size());
            }
            const HTREEITEM fileSceneItem = AddTreeItem(
                m_hSysTree, softwareItem,
                L"单选文件　（" + std::to_wstring(extTotal) + L" 种扩展名）",
                std::wstring(), fileSceneIndex + 1);

            for (int clusterIndex : ownClusters)
            {
                const shell::CensusCluster& cluster = m_report.clusters[clusterIndex];

                SysNode clusterNode;
                clusterNode.kind         = SysNode::Cluster;
                clusterNode.brand        = brand;
                clusterNode.clusterIndex = clusterIndex;
                m_sysNodes.push_back(clusterNode);
                const int clusterNodeIndex = static_cast<int>(m_sysNodes.size()) - 1;

                const HTREEITEM clusterItem = AddTreeItem(m_hSysTree, fileSceneItem,
                                                          cluster.label, std::wstring(),
                                                          clusterNodeIndex + 1);

                // 簇内条目：按 level 搭出子菜单层级
                std::vector<HTREEITEM> parents;
                for (int i = 0; i < static_cast<int>(cluster.items.size()); ++i)
                {
                    const shell::CensusItem& item = cluster.items[i];
                    if (item.separator) continue;

                    SysNode itemNode;
                    itemNode.kind             = SysNode::Item;
                    itemNode.brand            = brand;
                    itemNode.clusterIndex     = clusterIndex;
                    itemNode.clusterItemIndex = i;
                    itemNode.itemScene        = shell::SceneFile;
                    m_sysNodes.push_back(itemNode);
                    const int itemNodeIndex = static_cast<int>(m_sysNodes.size()) - 1;

                    const int level = item.level;
                    while (static_cast<int>(parents.size()) > level) parents.pop_back();
                    HTREEITEM parentItem = parents.empty() ? clusterItem : parents.back();

                    const HTREEITEM newItem = AddTreeItem(m_hSysTree, parentItem,
                                                          ItemDisplayText(item),
                                                          item.iconFile,
                                                          itemNodeIndex + 1);
                    parents.resize(level + 1);
                    parents[level] = newItem;
                }
                ::SendMessageW(m_hSysTree, TVM_EXPAND, TVE_COLLAPSE, reinterpret_cast<LPARAM>(clusterItem));
            }
            ::SendMessageW(m_hSysTree, TVM_EXPAND, TVE_COLLAPSE, reinterpret_cast<LPARAM>(fileSceneItem));
        }

        // ---- 其它三个场景 ----
        for (const auto& sceneEntry : ownScenes)
        {
            const unsigned int sceneBit = sceneEntry.first;
            SysNode sceneNode;
            sceneNode.kind  = SysNode::Scene;
            sceneNode.brand = brand;
            sceneNode.scene = sceneBit;
            m_sysNodes.push_back(sceneNode);
            const int sceneNodeIndex = static_cast<int>(m_sysNodes.size()) - 1;

            const HTREEITEM sceneItem = AddTreeItem(
                m_hSysTree, softwareItem,
                SceneName(sceneBit) + L"　（" + std::to_wstring(sceneEntry.second.size()) + L" 项）",
                std::wstring(), sceneNodeIndex + 1);

            std::vector<HTREEITEM> parents;
            for (int itemIndex : sceneEntry.second)
            {
                // 找到这个场景在报告里的下标（同一场景只出现一次）
                int sceneIndex = -1;
                for (int s = 0; s < static_cast<int>(m_report.scenes.size()); ++s)
                {
                    if (m_report.scenes[s].scene == sceneBit) { sceneIndex = s; break; }
                }
                if (sceneIndex < 0) continue;
                const shell::CensusScene& sourceScene = m_report.scenes[sceneIndex];
                if (itemIndex < 0 || itemIndex >= static_cast<int>(sourceScene.items.size())) continue;

                const shell::CensusItem& item = sourceScene.items[itemIndex];

                SysNode itemNode;
                itemNode.kind          = SysNode::Item;
                itemNode.brand         = brand;
                itemNode.sceneItemIndex = itemIndex;
                itemNode.itemScene     = sceneBit;
                m_sysNodes.push_back(itemNode);
                const int itemNodeIndex = static_cast<int>(m_sysNodes.size()) - 1;

                const int level = item.level;
                while (static_cast<int>(parents.size()) > level) parents.pop_back();
                HTREEITEM parentItem = parents.empty() ? sceneItem : parents.back();

                const HTREEITEM newItem = AddTreeItem(m_hSysTree, parentItem,
                                                      ItemDisplayText(item),
                                                      item.iconFile,
                                                      itemNodeIndex + 1);
                parents.resize(level + 1);
                parents[level] = newItem;
            }
            ::SendMessageW(m_hSysTree, TVM_EXPAND, TVE_COLLAPSE, reinterpret_cast<LPARAM>(sceneItem));
        }

        ::SendMessageW(m_hSysTree, TVM_EXPAND, TVE_COLLAPSE, reinterpret_cast<LPARAM>(softwareItem));
    }
}

// ============================================================================
// 右侧配置树
// ============================================================================

void ContextMenuSettingsDialog::ReloadConfig()
{
    m_config.groups.clear();
    m_config.entries.clear();
    shell::LoadMenuConfig(m_config);   // 失败就是空配置，界面照样能用
}

void ContextMenuSettingsDialog::RebuildCfgTree()
{
    if (m_hCfgTree == nullptr) return;

    ::SendMessageW(m_hCfgTree, TVM_DELETEITEM, 0, reinterpret_cast<LPARAM>(TVI_ROOT));
    m_cfgNodes.clear();

    std::vector<const shell::ConfigGroup*> groups;
    for (const shell::ConfigGroup& group : m_config.groups) groups.push_back(&group);
    std::sort(groups.begin(), groups.end(),
              [](const shell::ConfigGroup* a, const shell::ConfigGroup* b)
              { return a->order < b->order; });

    for (const shell::ConfigGroup* group : groups)
    {
        CfgNode node;
        node.kind = CfgNode::Group;
        node.id   = group->id;
        m_cfgNodes.push_back(node);
        const int nodeIndex = static_cast<int>(m_cfgNodes.size()) - 1;

        const HTREEITEM groupItem = AddTreeItem(m_hCfgTree, TVI_ROOT,
                                                group->title.empty() ? L"（未命名组）" : group->title,
                                                group->iconFile, nodeIndex + 1);

        // 递归挂条目（parentEntryId 为空的是组的直接子项）
        std::vector<std::pair<HTREEITEM, std::wstring>> pending;
        pending.push_back({ groupItem, std::wstring() });

        // 逐层处理，防止深层嵌套时递归爆栈
        for (int depth = 0; depth < 8 && !pending.empty(); ++depth)
        {
            std::vector<std::pair<HTREEITEM, std::wstring>> next;
            for (const auto& parent : pending)
            {
                std::vector<const shell::ConfigEntry*> children;
                for (const shell::ConfigEntry& entry : m_config.entries)
                {
                    if (entry.groupId == group->id && entry.parentEntryId == parent.second)
                    {
                        children.push_back(&entry);
                    }
                }
                std::sort(children.begin(), children.end(),
                          [](const shell::ConfigEntry* a, const shell::ConfigEntry* b)
                          { return a->order < b->order; });

                for (const shell::ConfigEntry* entry : children)
                {
                    CfgNode entryNode;
                    entryNode.kind = CfgNode::Entry;
                    entryNode.id   = entry->id;
                    m_cfgNodes.push_back(entryNode);
                    const int entryIndex = static_cast<int>(m_cfgNodes.size()) - 1;

                    const std::wstring text = entry->displayName.empty()
                                                  ? (entry->sourceText.empty() ? L"（无文字）"
                                                                               : entry->sourceText)
                                                  : entry->displayName;
                    const HTREEITEM entryItem = AddTreeItem(m_hCfgTree, parent.first, text,
                                                            entry->iconFile, entryIndex + 1);
                    next.push_back({ entryItem, entry->id });
                }
            }
            pending.swap(next);
        }

        ::SendMessageW(m_hCfgTree, TVM_EXPAND, TVE_EXPAND, reinterpret_cast<LPARAM>(groupItem));
    }

    // 默认选中第一个组，属性区不至于空着
    if (!m_config.groups.empty())
    {
        SelectCfgNodeById(m_config.groups.front().id);
    }
    else
    {
        LoadSelectionProperties();
    }
}

void ContextMenuSettingsDialog::SelectCfgNodeById(const std::wstring& id)
{
    if (m_hCfgTree == nullptr) return;

    // 深度优先遍历整棵树，找到 id 对应的节点并设为当前选中。
    // 树只有几层、节点也不多，直接递归最省事。
    std::function<HTREEITEM(HTREEITEM)> findItem = [&](HTREEITEM start) -> HTREEITEM
    {
        for (HTREEITEM current = start; current != nullptr;
             current = reinterpret_cast<HTREEITEM>(
                 ::SendMessageW(m_hCfgTree, TVM_GETNEXTITEM, TVGN_NEXT,
                                reinterpret_cast<LPARAM>(current))))
        {
            TVITEMW item = {};
            item.mask  = TVIF_PARAM;
            item.hItem = current;
            if (::SendMessageW(m_hCfgTree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)))
            {
                const int index = static_cast<int>(item.lParam) - 1;
                if (index >= 0 && index < static_cast<int>(m_cfgNodes.size()) &&
                    m_cfgNodes[index].id == id)
                {
                    return current;
                }
            }

            HTREEITEM child = reinterpret_cast<HTREEITEM>(
                ::SendMessageW(m_hCfgTree, TVM_GETNEXTITEM, TVGN_CHILD,
                               reinterpret_cast<LPARAM>(current)));
            if (child != nullptr)
            {
                HTREEITEM found = findItem(child);
                if (found != nullptr) return found;
            }
        }
        return nullptr;
    };

    HTREEITEM root = reinterpret_cast<HTREEITEM>(
        ::SendMessageW(m_hCfgTree, TVM_GETNEXTITEM, TVGN_ROOT, 0));
    if (root == nullptr) return;

    HTREEITEM target = findItem(root);
    if (target != nullptr)
    {
        ::SendMessageW(m_hCfgTree, TVM_SELECTITEM, TVGN_CARET,
                       reinterpret_cast<LPARAM>(target));
        ::SendMessageW(m_hCfgTree, TVM_ENSUREVISIBLE, 0,
                       reinterpret_cast<LPARAM>(target));
    }
}

// ============================================================================
// 预览
// ============================================================================

void ContextMenuSettingsDialog::ReleasePreviewBitmaps()
{
    for (HBITMAP bitmap : m_previewBitmaps)
    {
        if (bitmap != nullptr) ::DeleteObject(bitmap);
    }
    m_previewBitmaps.clear();
}

void ContextMenuSettingsDialog::RebuildPreview()
{
    if (m_hPreview == nullptr) return;

    ReleasePreviewBitmaps();
    m_previewRows.clear();
    ::SendMessageW(m_hPreview, LVM_DELETEALLITEMS, 0, 0);

    std::vector<const shell::ConfigGroup*> groups;
    for (const shell::ConfigGroup& group : m_config.groups) groups.push_back(&group);
    std::sort(groups.begin(), groups.end(),
              [](const shell::ConfigGroup* a, const shell::ConfigGroup* b)
              { return a->order < b->order; });

    for (const shell::ConfigGroup* group : groups)
    {
        PreviewRow row;
        row.text    = group->title;
        row.level   = 0;
        row.isGroup = true;
        row.iconFile = group->iconFile;
        m_previewRows.push_back(row);

        std::vector<std::pair<std::wstring, int>> pending = { { std::wstring(), 1 } };
        for (int depth = 0; depth < 8 && !pending.empty(); ++depth)
        {
            std::vector<std::pair<std::wstring, int>> next;
            for (const auto& parent : pending)
            {
                std::vector<const shell::ConfigEntry*> children;
                for (const shell::ConfigEntry& entry : m_config.entries)
                {
                    if (entry.groupId == group->id && entry.parentEntryId == parent.first)
                    {
                        children.push_back(&entry);
                    }
                }
                std::sort(children.begin(), children.end(),
                          [](const shell::ConfigEntry* a, const shell::ConfigEntry* b)
                          { return a->order < b->order; });

                for (const shell::ConfigEntry* entry : children)
                {
                    PreviewRow entryRow;
                    entryRow.text = entry->displayName.empty() ? entry->sourceText : entry->displayName;
                    if (entryRow.text.empty()) entryRow.text = L"（无文字）";
                    entryRow.level = parent.second;
                    entryRow.iconFile = entry->iconFile;
                    m_previewRows.push_back(entryRow);
                    next.push_back({ entry->id, parent.second + 1 });
                }
            }
            pending.swap(next);
        }
    }

    for (int i = 0; i < static_cast<int>(m_previewRows.size()); ++i)
    {
        m_previewBitmaps.push_back(
            LoadBitmapFile(m_previewRows[i].iconFile, Scale(kIconPixels)));

        LVITEMW lvItem = {};
        lvItem.mask  = LVIF_TEXT;
        lvItem.iItem = i;
        lvItem.pszText = const_cast<LPWSTR>(L"");
        ::SendMessageW(m_hPreview, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&lvItem));
    }

    if (m_previewRows.empty())
    {
        // 空配置给个说明行，避免一片空白
        m_previewRows.push_back({ L"（还没有任何菜单项：把左边的项目拖到右边试试）", 0, false, L"" });
        m_previewBitmaps.push_back(nullptr);
        LVITEMW lvItem = {};
        lvItem.mask = LVIF_TEXT;
        lvItem.pszText = const_cast<LPWSTR>(L"");
        ::SendMessageW(m_hPreview, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&lvItem));
    }

    ::InvalidateRect(m_hPreview, nullptr, TRUE);
}

// ============================================================================
// 选中项属性
// ============================================================================

bool ContextMenuSettingsDialog::SelectedCfgNode(CfgNode& out) const
{
    if (m_hCfgTree == nullptr) return false;

    HTREEITEM selected = reinterpret_cast<HTREEITEM>(
        ::SendMessageW(m_hCfgTree, TVM_GETNEXTITEM, TVGN_CARET, 0));
    if (selected == nullptr) return false;

    TVITEMW item = {};
    item.mask  = TVIF_PARAM;
    item.hItem = selected;
    if (!::SendMessageW(m_hCfgTree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)))
    {
        return false;
    }
    const int index = static_cast<int>(item.lParam) - 1;
    if (index < 0 || index >= static_cast<int>(m_cfgNodes.size())) return false;
    out = m_cfgNodes[index];
    return true;
}

void ContextMenuSettingsDialog::LoadSelectionProperties()
{
    m_fillingProperties = true;

    CfgNode node;
    const bool hasSelection = SelectedCfgNode(node);

    std::wstring name;
    std::wstring iconFile;
    unsigned int scenes = 0;
    std::wstring exts;

    if (hasSelection && node.kind == CfgNode::Group)
    {
        const shell::ConfigGroup* group = m_config.FindGroup(node.id);
        if (group != nullptr)
        {
            name     = group->title;
            iconFile = group->iconFile;
        }
    }
    else if (hasSelection)
    {
        const shell::ConfigEntry* entry = m_config.FindEntry(node.id);
        if (entry != nullptr)
        {
            name     = entry->displayName.empty() ? entry->sourceText : entry->displayName;
            iconFile = entry->iconFile;
            scenes   = entry->scope.scenes;
            for (const std::wstring& ext : entry->scope.exts)
            {
                if (!exts.empty()) exts += L" ";
                exts += ext;
            }
        }
    }

    ::SetWindowTextW(m_hNameEdit, name.c_str());
    ::SetWindowTextW(m_hExtsEdit, exts.c_str());
    ::SendMessageW(m_hCheckFile,  BM_SETCHECK,
                   (scenes & shell::SceneFile) ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(m_hCheckDir,   BM_SETCHECK,
                   (scenes & shell::SceneFolder) ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(m_hCheckBg,    BM_SETCHECK,
                   (scenes & shell::SceneBackground) ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(m_hCheckMulti, BM_SETCHECK,
                   (scenes & shell::SceneMultiple) ? BST_CHECKED : BST_UNCHECKED, 0);

    const bool isEntry = hasSelection && node.kind == CfgNode::Entry;
    ::EnableWindow(m_hNameEdit, hasSelection ? TRUE : FALSE);
    ::EnableWindow(m_hIconBtn, hasSelection ? TRUE : FALSE);
    ::EnableWindow(m_hIconClear, hasSelection ? TRUE : FALSE);
    ::EnableWindow(m_hCheckFile, isEntry ? TRUE : FALSE);
    ::EnableWindow(m_hCheckDir, isEntry ? TRUE : FALSE);
    ::EnableWindow(m_hCheckBg, isEntry ? TRUE : FALSE);
    ::EnableWindow(m_hCheckMulti, isEntry ? TRUE : FALSE);
    ::EnableWindow(m_hExtsEdit, isEntry ? TRUE : FALSE);

    // 图标按钮上顺便显示有没有图标，省得再开一列
    ::SetWindowTextW(m_hIconBtn,
                     iconFile.empty() ? L"设图标" : L"改图标");

    m_fillingProperties = false;
}

void ContextMenuSettingsDialog::ApplyNameFromEdit()
{
    if (m_fillingProperties) return;

    CfgNode node;
    if (!SelectedCfgNode(node)) return;

    const std::wstring text = Trim([&]() -> std::wstring
    {
        const int length = ::GetWindowTextLengthW(m_hNameEdit);
        std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
        ::GetWindowTextW(m_hNameEdit, &buffer[0], length + 1);
        buffer.resize(static_cast<size_t>(length));
        return buffer;
    }());

    if (node.kind == CfgNode::Group)
    {
        shell::ConfigGroup* group = m_config.FindGroup(node.id);
        if (group == nullptr) return;
        if (group->title == text) return;
        group->title = text;
    }
    else
    {
        shell::ConfigEntry* entry = m_config.FindEntry(node.id);
        if (entry == nullptr) return;
        // 改回和原始文字一样时，清空 displayName（以后软件升级文字变了也能跟着变）
        entry->displayName = (text == entry->sourceText) ? std::wstring() : text;
    }

    SaveConfig();
    RebuildCfgTree();
    SelectCfgNodeById(node.id);
    RebuildPreview();
}

void ContextMenuSettingsDialog::ApplyExtsFromEdit()
{
    if (m_fillingProperties) return;

    CfgNode node;
    if (!SelectedCfgNode(node) || node.kind != CfgNode::Entry) return;

    shell::ConfigEntry* entry = m_config.FindEntry(node.id);
    if (entry == nullptr) return;

    const int length = ::GetWindowTextLengthW(m_hExtsEdit);
    std::wstring raw(static_cast<size_t>(length) + 1, L'\0');
    ::GetWindowTextW(m_hExtsEdit, &raw[0], length + 1);
    raw.resize(static_cast<size_t>(length));

    std::vector<std::wstring> exts;
    std::wstring token;
    for (size_t i = 0; i <= raw.size(); ++i)
    {
        const wchar_t ch = (i < raw.size()) ? raw[i] : L' ';
        if (ch == L' ' || ch == L',' || ch == L';' || ch == L'，' || ch == L'；' || ch == L'\t')
        {
            std::wstring trimmed = Trim(token);
            token.clear();
            if (trimmed.empty()) continue;
            if (trimmed.front() != L'.') trimmed.insert(trimmed.begin(), L'.');
            for (wchar_t& c : trimmed) c = static_cast<wchar_t>(::towlower(c));
            bool duplicate = false;
            for (const std::wstring& existing : exts)
            {
                if (existing == trimmed) { duplicate = true; break; }
            }
            if (!duplicate) exts.push_back(trimmed);
        }
        else
        {
            token.push_back(ch);
        }
    }

    if (entry->scope.exts == exts) return;
    entry->scope.exts = exts;
    SaveConfig();
    RebuildCfgTree();
    SelectCfgNodeById(node.id);
}

void ContextMenuSettingsDialog::ApplyScopeFromChecks()
{
    if (m_fillingProperties) return;

    CfgNode node;
    if (!SelectedCfgNode(node) || node.kind != CfgNode::Entry) return;

    shell::ConfigEntry* entry = m_config.FindEntry(node.id);
    if (entry == nullptr) return;

    unsigned int scenes = 0;
    if (::SendMessageW(m_hCheckFile, BM_GETCHECK, 0, 0) == BST_CHECKED)  scenes |= shell::SceneFile;
    if (::SendMessageW(m_hCheckDir, BM_GETCHECK, 0, 0) == BST_CHECKED)   scenes |= shell::SceneFolder;
    if (::SendMessageW(m_hCheckBg, BM_GETCHECK, 0, 0) == BST_CHECKED)    scenes |= shell::SceneBackground;
    if (::SendMessageW(m_hCheckMulti, BM_GETCHECK, 0, 0) == BST_CHECKED) scenes |= shell::SceneMultiple;

    if (entry->scope.scenes == scenes) return;
    entry->scope.scenes = scenes;

    // 不再对"单选文件"生效时，扩展名约束没意义，顺手清掉
    if ((scenes & shell::SceneFile) == 0) entry->scope.exts.clear();

    SaveConfig();
}

void ContextMenuSettingsDialog::SaveConfig()
{
    if (!shell::SaveMenuConfig(m_config))
    {
        ::SetWindowTextW(m_hStatus, L"保存失败：写 menu-config.json 出错");
        return;
    }
    ::SetWindowTextW(m_hStatus, (L"配置已保存（" + std::to_wstring(m_config.entries.size()) +
                                 L" 项，菜单配置）　" + NowText()).c_str());
}

// ============================================================================
// 拖拽
// ============================================================================

int ContextMenuSettingsDialog::SelectedSysNodeIndex() const
{
    if (m_hSysTree == nullptr) return -1;
    HTREEITEM selected = reinterpret_cast<HTREEITEM>(
        ::SendMessageW(m_hSysTree, TVM_GETNEXTITEM, TVGN_CARET, 0));
    if (selected == nullptr) return -1;

    TVITEMW item = {};
    item.mask  = TVIF_PARAM;
    item.hItem = selected;
    if (!::SendMessageW(m_hSysTree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item))) return -1;
    const int index = static_cast<int>(item.lParam) - 1;
    return (index >= 0 && index < static_cast<int>(m_sysNodes.size())) ? index : -1;
}

void ContextMenuSettingsDialog::BeginDrag(HTREEITEM sysItem)
{
    m_dragging = true;
    ::SetCapture(m_hWnd);

    TVITEMW item = {};
    item.mask  = TVIF_PARAM | TVIF_TEXT;
    item.hItem = sysItem;
    wchar_t textBuffer[256] = {};
    item.pszText = textBuffer;
    item.cchTextMax = ARRAYSIZE(textBuffer);
    std::wstring hint = L"拖拽中…";
    if (::SendMessageW(m_hSysTree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)))
    {
        hint = L"拖拽中：" + std::wstring(item.pszText);
    }
    ::SetWindowTextW(m_hStatus, (hint + L"　放到右侧的组或条目上").c_str());
}

void ContextMenuSettingsDialog::UpdateDragHighlight(POINT screenPt)
{
    int cfgIndex = -1;
    HWND hit = HitTestCfgTree(screenPt, &cfgIndex);
    if (hit == nullptr)
    {
        ::SendMessageW(m_hCfgTree, TVM_SETINSERTMARK, FALSE, 0);
        return;
    }
    if (cfgIndex < 0)
    {
        // 空白处：插入标记挂在最后一个节点后面
        ::SendMessageW(m_hCfgTree, TVM_SETINSERTMARK, FALSE, 0);
        return;
    }
    ::SendMessageW(m_hCfgTree, TVM_SETINSERTMARK, TRUE, 0);
}

HWND ContextMenuSettingsDialog::HitTestCfgTree(POINT screenPt, int* cfgIndex) const
{
    if (cfgIndex != nullptr) *cfgIndex = -1;

    RECT treeRect = {};
    ::GetWindowRect(m_hCfgTree, &treeRect);
    if (!::PtInRect(&treeRect, screenPt)) return nullptr;

    POINT client = screenPt;
    ::ScreenToClient(m_hCfgTree, &client);

    TVHITTESTINFO hit = {};
    hit.pt = client;
    ::SendMessageW(m_hCfgTree, TVM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit));
    if (hit.hItem != nullptr)
    {
        TVITEMW item = {};
        item.mask  = TVIF_PARAM;
        item.hItem = hit.hItem;
        if (::SendMessageW(m_hCfgTree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)))
        {
            const int index = static_cast<int>(item.lParam) - 1;
            if (index >= 0 && index < static_cast<int>(m_cfgNodes.size()) && cfgIndex != nullptr)
            {
                *cfgIndex = index;
            }
        }
    }
    return m_hCfgTree;
}

void ContextMenuSettingsDialog::BuildDropItems(int sysNodeIndex, std::vector<DropItem>& out) const
{
    if (sysNodeIndex < 0 || sysNodeIndex >= static_cast<int>(m_sysNodes.size())) return;
    const SysNode& node = m_sysNodes[sysNodeIndex];

    auto fill = [&out](const shell::CensusItem& item, int baseLevel, int level,
                       unsigned int scenes, const std::vector<std::wstring>& exts)
    {
        if (item.separator) return;
        DropItem drop;
        drop.text     = item.text;
        drop.pathKey  = item.pathKey;
        drop.brand    = item.brand;
        drop.verb     = item.verb;
        drop.iconFile = item.iconFile;
        drop.level    = level - baseLevel;
        drop.scenes   = scenes;
        drop.exts     = exts;
        if (drop.level < 0) drop.level = 0;
        out.push_back(drop);
    };

    auto appendCluster = [&](int clusterIndex, int startItemIndex)
    {
        const shell::CensusCluster& cluster = m_report.clusters[clusterIndex];
        if (startItemIndex < 0)
        {
            for (int i = 0; i < static_cast<int>(cluster.items.size()); ++i)
            {
                fill(cluster.items[i], 0, cluster.items[i].level,
                     shell::SceneFile, cluster.exts);
            }
            return;
        }

        // 只取某一条目的子树：从它开始，直到层级回到不深于它
        const int baseLevel = cluster.items[startItemIndex].level;
        for (int i = startItemIndex; i < static_cast<int>(cluster.items.size()); ++i)
        {
            const shell::CensusItem& item = cluster.items[i];
            if (i > startItemIndex && item.level <= baseLevel) break;
            fill(item, baseLevel, item.level, shell::SceneFile, cluster.exts);
        }
    };

    switch (node.kind)
    {
    case SysNode::Item:
        if (node.itemScene == shell::SceneFile && node.clusterIndex >= 0)
        {
            appendCluster(node.clusterIndex, node.clusterItemIndex);
        }
        else
        {
            int sceneIndex = -1;
            for (int s = 0; s < static_cast<int>(m_report.scenes.size()); ++s)
            {
                if (m_report.scenes[s].scene == node.itemScene) { sceneIndex = s; break; }
            }
            if (sceneIndex < 0) return;
            const shell::CensusScene& scene = m_report.scenes[sceneIndex];
            if (node.sceneItemIndex < 0 ||
                node.sceneItemIndex >= static_cast<int>(scene.items.size())) return;

            const int baseLevel = scene.items[node.sceneItemIndex].level;
            for (int i = node.sceneItemIndex; i < static_cast<int>(scene.items.size()); ++i)
            {
                const shell::CensusItem& item = scene.items[i];
                if (i > node.sceneItemIndex && item.level <= baseLevel) break;
                if (item.separator) continue;
                fill(item, baseLevel, item.level, node.itemScene, {});
            }
        }
        break;

    case SysNode::Cluster:
        appendCluster(node.clusterIndex, -1);
        break;

    case SysNode::Scene:
        if (node.scene == shell::SceneFile)
        {
            for (int i = 0; i < static_cast<int>(m_report.clusters.size()); ++i)
            {
                if (m_report.clusters[i].brand == node.brand) appendCluster(i, -1);
            }
        }
        else
        {
            for (const shell::CensusScene& scene : m_report.scenes)
            {
                if (scene.scene != node.scene) continue;
                for (const shell::CensusItem& item : scene.items)
                {
                    if (item.separator) continue;
                    const std::wstring brand = item.brand.empty() ? L"其它" : item.brand;
                    if (brand != node.brand) continue;
                    fill(item, 0, item.level, node.scene, {});
                }
                break;
            }
        }
        break;

    case SysNode::Software:
        for (int i = 0; i < static_cast<int>(m_report.clusters.size()); ++i)
        {
            if (m_report.clusters[i].brand == node.brand) appendCluster(i, -1);
        }
        for (const shell::CensusScene& scene : m_report.scenes)
        {
            for (const shell::CensusItem& item : scene.items)
            {
                if (item.separator) continue;
                const std::wstring brand = item.brand.empty() ? L"其它" : item.brand;
                if (brand != node.brand) continue;
                fill(item, 0, item.level, scene.scene, {});
            }
        }
        break;
    }
}

void ContextMenuSettingsDialog::InsertDropItems(const std::vector<DropItem>& items,
                                                const std::wstring& groupId,
                                                const std::wstring& parentEntryId)
{
    if (items.empty()) return;

    // 同层顺序：接在现有兄弟后面
    std::vector<std::pair<std::wstring, int>> lastOrder;   // 父条目 ID → 当前最大 order
    for (const shell::ConfigEntry& entry : m_config.entries)
    {
        if (entry.groupId != groupId) continue;
        bool found = false;
        for (auto& pair : lastOrder)
        {
            if (pair.first == entry.parentEntryId)
            {
                pair.second = std::max(pair.second, entry.order);
                found = true;
                break;
            }
        }
        if (!found) lastOrder.push_back({ entry.parentEntryId, entry.order });
    }

    auto nextOrder = [&lastOrder](const std::wstring& parent) -> int
    {
        for (auto& pair : lastOrder)
        {
            if (pair.first == parent) { pair.second += 1; return pair.second; }
        }
        lastOrder.push_back({ parent, 0 });
        return 0;
    };

    std::vector<std::wstring> idStack;   // 相对深度 → 条目 ID

    for (const DropItem& drop : items)
    {
        int level = drop.level;
        if (level > static_cast<int>(idStack.size())) level = static_cast<int>(idStack.size());
        if (level < 0) level = 0;
        while (static_cast<int>(idStack.size()) > level) idStack.pop_back();

        const std::wstring parent = idStack.empty() ? parentEntryId : idStack.back();

        shell::ConfigEntry entry;
        entry.id            = m_config.NewEntryId();
        entry.groupId       = groupId;
        entry.parentEntryId = parent;
        entry.pathKey       = drop.pathKey;
        entry.brand         = drop.brand;
        entry.sourceText    = drop.text;
        entry.displayName   = std::wstring();
        entry.iconFile      = drop.iconFile;
        entry.scope.scenes  = drop.scenes;
        entry.scope.exts    = drop.exts;
        entry.order         = nextOrder(parent);
        m_config.entries.push_back(entry);

        idStack.resize(level + 1);
        idStack[level] = entry.id;
    }

    SaveConfig();
    RebuildCfgTree();
    RebuildPreview();
}

void ContextMenuSettingsDialog::FinishDrag(POINT screenPt)
{
    const int sysIndex = SelectedSysNodeIndex();
    if (sysIndex < 0) return;

    int cfgIndex = -1;
    HWND hit = HitTestCfgTree(screenPt, &cfgIndex);

    // 没拖到右侧配置树：放弃
    if (hit == nullptr)
    {
        ::SetWindowTextW(m_hStatus, L"没有放到右侧，已取消");
        return;
    }

    std::wstring groupId;
    std::wstring parentEntryId;

    if (cfgIndex < 0)
    {
        // 空白区：放到当前选中的组；没有组就自动建一个
        CfgNode selected;
        if (SelectedCfgNode(selected) && selected.kind == CfgNode::Group)
        {
            groupId = selected.id;
        }
        else if (!m_config.groups.empty())
        {
            groupId = m_config.groups.back().id;
        }
        else
        {
            shell::ConfigGroup group;
            group.id    = m_config.NewGroupId();
            group.title = L"我的右键菜单";
            group.order = 0;
            m_config.groups.push_back(group);
            groupId = group.id;
        }
    }
    else
    {
        const CfgNode& target = m_cfgNodes[cfgIndex];
        if (target.kind == CfgNode::Group)
        {
            groupId = target.id;
        }
        else
        {
            const shell::ConfigEntry* entry = m_config.FindEntry(target.id);
            if (entry == nullptr) return;
            groupId = entry->groupId;
            parentEntryId = entry->id;
        }
    }

    std::vector<DropItem> items;
    BuildDropItems(sysIndex, items);
    if (items.empty())
    {
        ::SetWindowTextW(m_hStatus, L"这个节点下没有可拖走的菜单项");
        return;
    }

    InsertDropItems(items, groupId, parentEntryId);
    ::SetWindowTextW(m_hStatus,
                     (L"已加入 " + std::to_wstring(items.size()) + L" 项　" + NowText()).c_str());
}

// ============================================================================
// 普查
// ============================================================================

void ContextMenuSettingsDialog::StartCensus()
{
    if (m_censusRunning) return;

    m_censusRunning = true;
    m_censusCancel  = false;

    if (m_hRefresh != nullptr)
    {
        ::EnableWindow(m_hRefresh, FALSE);
        ::EnableWindow(m_hSupplement, FALSE);
    }
    ::SetWindowTextW(m_hStatus, L"正在普查本机所有扩展名的右键菜单（约 1~3 分钟，可继续配置）…");

    HWND window = m_hWnd;
    m_censusThread = std::thread([window]()
    {
        std::vector<std::wstring> extensions = shell::EnumerateExtensions();

        wchar_t exePath[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);

        std::vector<shell::CensusExtension> results;
        shell::RunCensusInChildProcesses(exePath, extensions, results,
            [window](int done, int total)
            {
                ::PostMessageW(window, WM_APP + 2, static_cast<WPARAM>(done),
                               static_cast<LPARAM>(total));
            });

        shell::CensusReport* report = new shell::CensusReport();
        report->scannedAt  = NowText();
        report->extensions = std::move(results);
        shell::CollectNonFileScenes(report->scenes);
        shell::BuildClusters(*report);

        ::PostMessageW(window, WM_APP + 3, 0, reinterpret_cast<LPARAM>(report));
    });
    m_censusThread.detach();
}

void ContextMenuSettingsDialog::OnCensusDone(shell::CensusReport* report)
{
    m_censusRunning = false;

    if (m_hRefresh != nullptr)
    {
        ::EnableWindow(m_hRefresh, TRUE);
        ::EnableWindow(m_hSupplement, TRUE);
    }

    if (report == nullptr) return;

    m_report = std::move(*report);
    delete report;
    m_hasReport = true;

    shell::SaveCensus(m_report);
    RebuildSysTree();
    RebuildCfgTree();
    RebuildPreview();

    std::wstring status = L"普查完成：" + std::to_wstring(m_report.extensions.size()) +
                          L" 种扩展名，" + std::to_wstring(m_report.clusters.size()) +
                          L" 个菜单簇";
    const int broken = m_report.BrokenCount();
    if (broken > 0)
    {
        status += L"，" + std::to_wstring(broken) + L" 种采集失败（不影响其它）";
    }
    ::SetWindowTextW(m_hStatus, (status + L"　" + m_report.scannedAt).c_str());
}

// ============================================================================
// 消息
// ============================================================================

LRESULT ContextMenuSettingsDialog::HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SIZE:
        Layout();
        return 0;

    case WM_MEASUREITEM:
    {
        MEASUREITEMSTRUCT* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (measure != nullptr && measure->CtlID == static_cast<UINT>(kIdPreview))
        {
            measure->itemHeight = static_cast<UINT>(Scale(kPreviewRowH));
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (draw != nullptr && draw->CtlID == static_cast<UINT>(kIdPreview) &&
            draw->itemID != static_cast<UINT>(-1) &&
            draw->itemID < m_previewRows.size())
        {
            ::FillRect(draw->hDC, &draw->rcItem,
                       static_cast<HBRUSH>(::GetSysColorBrush(
                           (draw->itemState & ODS_SELECTED) ? COLOR_HIGHLIGHT : COLOR_WINDOW)));

            const PreviewRow& row = m_previewRows[draw->itemID];
            const int indent = Scale(8) + Scale(16) * row.level;

            HBITMAP bitmap = (draw->itemID < m_previewBitmaps.size())
                                 ? m_previewBitmaps[draw->itemID] : nullptr;
            if (bitmap != nullptr)
            {
                HDC memDc = ::CreateCompatibleDC(draw->hDC);
                HGDIOBJ oldBitmap = ::SelectObject(memDc, bitmap);
                BITMAP info = {};
                ::GetObjectW(bitmap, sizeof(info), &info);
                const int height = draw->rcItem.bottom - draw->rcItem.top - Scale(4);
                const int width = (info.bmHeight > 0)
                                      ? info.bmWidth * height / info.bmHeight : height;
                ::SetStretchBltMode(draw->hDC, HALFTONE);
                ::StretchBlt(draw->hDC, draw->rcItem.left + indent,
                             draw->rcItem.top + Scale(2), width, height,
                             memDc, 0, 0, info.bmWidth, info.bmHeight, SRCCOPY);
                ::SelectObject(memDc, oldBitmap);
                ::DeleteDC(memDc);
            }

            ::SetBkMode(draw->hDC, TRANSPARENT);
            ::SetTextColor(draw->hDC, (draw->itemState & ODS_SELECTED)
                                          ? ::GetSysColor(COLOR_HIGHLIGHTTEXT)
                                          : (row.isGroup ? kGroupTextColor : kTextColor));
            HGDIOBJ oldFont = ::SelectObject(draw->hDC, m_font);
            RECT textRect = draw->rcItem;
            textRect.left += indent + Scale(kIconPixels) + Scale(6);
            ::DrawTextW(draw->hDC, row.text.c_str(), -1, &textRect,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            ::SelectObject(draw->hDC, oldFont);
            return TRUE;
        }
        break;
    }

    case WM_NOTIFY:
    {
        NMHDR* header = reinterpret_cast<NMHDR*>(lParam);
        if (header == nullptr) break;

        if (header->hwndFrom == m_hSysTree && header->code == TVN_BEGINDRAG)
        {
            const NMTREEVIEWW* drag = reinterpret_cast<const NMTREEVIEWW*>(lParam);
            BeginDrag(drag->itemNew.hItem);
            return 0;
        }
        if (header->hwndFrom == m_hCfgTree && header->code == TVN_SELCHANGED)
        {
            LoadSelectionProperties();
            return 0;
        }
        if (header->hwndFrom == m_hSysTree && header->code == TVN_SELCHANGED)
        {
            // 左侧选中时给个提示，告诉用户能拖
            const NMTREEVIEWW* change = reinterpret_cast<const NMTREEVIEWW*>(lParam);
            if (change->itemNew.hItem != nullptr)
            {
                ::SetWindowTextW(m_hStatus, L"提示：按住这一项拖到右侧的组或条目上，即可加入我们的菜单");
            }
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE:
        if (m_dragging)
        {
            POINT screenPt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ::ClientToScreen(hWnd, &screenPt);
            UpdateDragHighlight(screenPt);
            return 0;
        }
        break;

    case WM_LBUTTONUP:
        if (m_dragging)
        {
            POINT screenPt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ::ClientToScreen(hWnd, &screenPt);
            ::SendMessageW(m_hCfgTree, TVM_SETINSERTMARK, FALSE, 0);
            m_dragging = false;
            ::ReleaseCapture();
            FinishDrag(screenPt);
            return 0;
        }
        break;

    case WM_COMMAND:
    {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);

        if (id == kIdRefresh)
        {
            StartCensus();
            return 0;
        }

        if (id == kIdSupplement)
        {
            // 一键补图标：按品牌反查程序/DLL，取真图标给所有还缺图的条目
            if (!m_hasReport)
            {
                ::SetWindowTextW(m_hStatus, L"还没有普查数据，先刷新系统菜单");
                return 0;
            }
            std::vector<std::wstring> brands;
            for (const shell::CensusCluster& cluster : m_report.clusters)
            {
                if (cluster.brand == L"系统") continue;
                bool exists = false;
                for (const std::wstring& brand : brands)
                {
                    if (brand == cluster.brand) { exists = true; break; }
                }
                if (!exists) brands.push_back(cluster.brand);
            }

            int filledBrands = 0;
            for (const std::wstring& brand : brands)
            {
                const std::wstring icon = shell::ExtractBrandIcon(brand);
                if (icon.empty()) continue;
                ++filledBrands;

                // 给这个品牌下"原本没有图标"的条目补上
                for (shell::CensusCluster& cluster : m_report.clusters)
                {
                    if (cluster.brand != brand) continue;
                    for (shell::CensusItem& item : cluster.items)
                    {
                        if (item.iconFile.empty()) item.iconFile = icon;
                    }
                }
                for (shell::CensusScene& scene : m_report.scenes)
                {
                    for (shell::CensusItem& item : scene.items)
                    {
                        if (item.iconFile.empty() && item.brand == brand) item.iconFile = icon;
                    }
                }
            }

            if (filledBrands == 0)
            {
                ::SetWindowTextW(m_hStatus, L"没有找到可提取图标的程序（可能这个软件的图标本来就由菜单绘制）");
                return 0;
            }
            shell::SaveCensus(m_report);
            RebuildSysTree();
            RebuildCfgTree();
            RebuildPreview();
            ::SetWindowTextW(m_hStatus,
                             (L"已为 " + std::to_wstring(filledBrands) + L" 个软件补齐品牌图标").c_str());
            return 0;
        }

        if (id == kIdSave)
        {
            SaveConfig();
            return 0;
        }

        if (id == kIdNewGroup)
        {
            shell::ConfigGroup group;
            group.id = m_config.NewGroupId();
            int maxOrder = -1;
            for (const shell::ConfigGroup& existing : m_config.groups)
            {
                maxOrder = std::max(maxOrder, existing.order);
            }
            group.order = maxOrder + 1;
            group.title = L"新菜单组";
            m_config.groups.push_back(group);
            SaveConfig();
            RebuildCfgTree();
            SelectCfgNodeById(group.id);
            RebuildPreview();
            return 0;
        }

        if (id == kIdDelete)
        {
            CfgNode node;
            if (!SelectedCfgNode(node))
            {
                ::SetWindowTextW(m_hStatus, L"先选中要删除的组或条目");
                return 0;
            }

            if (node.kind == CfgNode::Group)
            {
                // 删组 = 连同组里的条目一起删
                std::vector<shell::ConfigEntry> kept;
                for (const shell::ConfigEntry& entry : m_config.entries)
                {
                    if (entry.groupId != node.id) kept.push_back(entry);
                }
                m_config.entries.swap(kept);

                std::vector<shell::ConfigGroup> groups;
                for (const shell::ConfigGroup& group : m_config.groups)
                {
                    if (group.id != node.id) groups.push_back(group);
                }
                m_config.groups.swap(groups);
            }
            else
            {
                // 删条目 = 连同它的子条目一起删（先收集所有后代）
                std::vector<std::wstring> doomed = { node.id };
                bool grew = true;
                while (grew)
                {
                    grew = false;
                    for (const shell::ConfigEntry& entry : m_config.entries)
                    {
                        bool parentDoomed = false;
                        for (const std::wstring& doomedId : doomed)
                        {
                            if (entry.parentEntryId == doomedId) { parentDoomed = true; break; }
                        }
                        if (!parentDoomed) continue;
                        bool already = false;
                        for (const std::wstring& doomedId : doomed)
                        {
                            if (entry.id == doomedId) { already = true; break; }
                        }
                        if (!already) { doomed.push_back(entry.id); grew = true; }
                    }
                }
                std::vector<shell::ConfigEntry> kept;
                for (const shell::ConfigEntry& entry : m_config.entries)
                {
                    bool remove = false;
                    for (const std::wstring& doomedId : doomed)
                    {
                        if (entry.id == doomedId) { remove = true; break; }
                    }
                    if (!remove) kept.push_back(entry);
                }
                m_config.entries.swap(kept);
            }

            SaveConfig();
            RebuildCfgTree();
            RebuildPreview();
            return 0;
        }

        if (id == kIdMoveUp || id == kIdMoveDown)
        {
            CfgNode node;
            if (!SelectedCfgNode(node)) return 0;
            const int delta = (id == kIdMoveUp) ? -1 : 1;

            if (node.kind == CfgNode::Group)
            {
                shell::ConfigGroup* group = m_config.FindGroup(node.id);
                if (group == nullptr) return 0;

                // 找紧挨着的那个兄弟：上移=比它小的里面最大的；下移=比它大的里面最小的
                shell::ConfigGroup* neighbour = nullptr;
                for (shell::ConfigGroup& candidate : m_config.groups)
                {
                    if (candidate.id == node.id) continue;
                    const bool eligible = (delta < 0) ? (candidate.order < group->order)
                                                     : (candidate.order > group->order);
                    if (!eligible) continue;
                    if (neighbour == nullptr ||
                        (delta < 0 && candidate.order > neighbour->order) ||
                        (delta > 0 && candidate.order < neighbour->order))
                    {
                        neighbour = &candidate;
                    }
                }
                if (neighbour == nullptr) return 0;
                std::swap(group->order, neighbour->order);
            }
            else
            {
                shell::ConfigEntry* entry = m_config.FindEntry(node.id);
                if (entry == nullptr) return 0;

                shell::ConfigEntry* neighbour = nullptr;
                for (shell::ConfigEntry& candidate : m_config.entries)
                {
                    if (candidate.id == node.id) continue;
                    if (candidate.groupId != entry->groupId) continue;
                    if (candidate.parentEntryId != entry->parentEntryId) continue;
                    const bool eligible = (delta < 0) ? (candidate.order < entry->order)
                                                     : (candidate.order > entry->order);
                    if (!eligible) continue;
                    if (neighbour == nullptr ||
                        (delta < 0 && candidate.order > neighbour->order) ||
                        (delta > 0 && candidate.order < neighbour->order))
                    {
                        neighbour = &candidate;
                    }
                }
                if (neighbour == nullptr) return 0;
                std::swap(entry->order, neighbour->order);
            }

            SaveConfig();
            RebuildCfgTree();
            SelectCfgNodeById(node.id);
            RebuildPreview();
            return 0;
        }

        if (id == kIdIconBtn)
        {
            CfgNode node;
            if (!SelectedCfgNode(node)) return 0;

            wchar_t path[MAX_PATH] = {};
            OPENFILENAMEW dialog = {};
            dialog.hwndOwner = m_hWnd;
            dialog.lpstrFilter = L"图标文件 (*.ico;*.bmp)\0*.ico;*.bmp\0所有文件\0*.*\0";
            dialog.lpstrFile = path;
            dialog.nMaxFile = MAX_PATH;
            dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (!::GetOpenFileNameW(&dialog)) return 0;

            // 用户选的图标复制到 menu-icons 目录，以后原文件删了也不影响
            std::wstring source = path;
            const std::wstring dir = shell::IconDir();
            std::wstring stored;
            if (!dir.empty())
            {
                const size_t dot = source.find_last_of(L'.');
                std::wstring extension = (dot != std::wstring::npos) ? source.substr(dot) : L".bmp";
                const unsigned int salt = ::GetTickCount();
                stored = L"u" + std::to_wstring(salt) + extension;
                const std::wstring target = dir + L"\\" + stored;
                if (::CopyFileW(source.c_str(), target.c_str(), FALSE)) { }
                else stored.clear();
            }
            if (stored.empty()) stored = source;   // 复制失败就用绝对路径顶着

            if (node.kind == CfgNode::Group)
            {
                shell::ConfigGroup* group = m_config.FindGroup(node.id);
                if (group == nullptr) return 0;
                group->iconFile = stored;
            }
            else
            {
                shell::ConfigEntry* entry = m_config.FindEntry(node.id);
                if (entry == nullptr) return 0;
                entry->iconFile = stored;
            }
            SaveConfig();
            RebuildCfgTree();
            SelectCfgNodeById(node.id);
            RebuildPreview();
            return 0;
        }

        if (id == kIdIconClear)
        {
            CfgNode node;
            if (!SelectedCfgNode(node)) return 0;
            if (node.kind == CfgNode::Group)
            {
                shell::ConfigGroup* group = m_config.FindGroup(node.id);
                if (group != nullptr) group->iconFile.clear();
            }
            else
            {
                shell::ConfigEntry* entry = m_config.FindEntry(node.id);
                if (entry != nullptr) entry->iconFile.clear();
            }
            SaveConfig();
            RebuildCfgTree();
            SelectCfgNodeById(node.id);
            RebuildPreview();
            return 0;
        }

        if (id == kIdNameEdit && code == EN_KILLFOCUS)
        {
            ApplyNameFromEdit();
            return 0;
        }

        if (id == kIdExtsEdit && code == EN_KILLFOCUS)
        {
            ApplyExtsFromEdit();
            return 0;
        }

        if ((id == kIdCheckFile || id == kIdCheckDir || id == kIdCheckBg || id == kIdCheckMulti) &&
            code == BN_CLICKED)
        {
            ApplyScopeFromChecks();
            return 0;
        }

        if (id == IDCANCEL || id == IDOK)
        {
            ::DestroyWindow(hWnd);
            return 0;
        }
        break;
    }

    case WM_APP + 2:
    {
        // 普查进度
        const int done = static_cast<int>(wParam);
        const int total = static_cast<int>(lParam);
        wchar_t buffer[128] = {};
        swprintf_s(buffer, L"正在普查右键菜单… %d / %d 批（可继续拖拽配置）", done, total);
        ::SetWindowTextW(m_hStatus, buffer);
        return 0;
    }

    case WM_APP + 3:
    {
        OnCensusDone(reinterpret_cast<shell::CensusReport*>(lParam));
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            ::DestroyWindow(hWnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        ::DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        m_censusCancel = true;
        ClearIconList();
        ReleasePreviewBitmaps();
        m_hWnd = nullptr;
        return 0;
    }

    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace ui
