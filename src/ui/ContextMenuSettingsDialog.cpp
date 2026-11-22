// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/ContextMenuSettingsDialog.cpp
//
// 「配置右键菜单」窗口实现。界面说明见 ContextMenuSettingsDialog.h。
//
// 数据链路：
//   · 打开窗口：先读 menu-cache.json 缓存，有就直接展示；
//   · 点「刷新」：EnumerateRegistry 扫注册表 → CollectDynamicItems 运行时采集
//     → SaveCache 落缓存 → 重建树。
//
// 一期只做查看，窗口内不写回配置、不执行菜单命令。
// ============================================================================

#include "ui/ContextMenuSettingsDialog.h"

#include "core/Logger.h"
#include "shell/ContextMenuEnumerator.h"
#include "shell/ContextMenuCollector.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlwapi.h>

#include <string>
#include <unordered_map>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace ui
{

namespace
{

const wchar_t* const kClassName = L"FileExplorerContextMenuSettingsDialog";

const int kControlIdStatus  = 2201;
const int kControlIdRefresh = 2202;
const int kControlIdTree    = 2203;

// 窗口客户区尺寸（逻辑像素）
const int kDialogWidth  = 720;
const int kDialogHeight = 480;
const int kPadding      = 14;
const int kButtonHeight = 28;
const int kButtonWidth  = 96;
const int kTopRowHeight = 34;

// 往树里插一个节点；返回节点句柄
HTREEITEM InsertNode(HWND tree, HTREEITEM parent, const std::wstring& text, LPARAM lParam)
{
    TVINSERTSTRUCTW insert = {};
    insert.hParent      = parent;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask    = TVIF_TEXT | TVIF_PARAM;
    insert.item.pszText = const_cast<LPWSTR>(text.data());
    insert.item.lParam  = lParam;
    return reinterpret_cast<HTREEITEM>(
        ::SendMessageW(tree, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&insert)));
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
    if (m_font != nullptr)
    {
        ::DeleteObject(m_font);
        m_font = nullptr;
    }
}

// ============================================================================
// 窗口类
// ============================================================================

bool ContextMenuSettingsDialog::EnsureClassRegistered()
{
    static bool registered = false;
    if (registered)
    {
        return true;
    }

    if (!RegisterWindowClass(kClassName, ::GetModuleHandleW(nullptr), CS_DBLCLKS))
    {
        return false;
    }

    registered = true;
    return true;
}

// ============================================================================
// 显示（模态）
// ============================================================================

void ContextMenuSettingsDialog::ShowModal()
{
    // 先读缓存；没有缓存就先跑一次采集
    if (!shell::LoadCache(m_report) || m_report.groups.empty())
    {
        m_report = shell::EnumerateRegistry();
        shell::CollectDynamicItems(m_report);
        shell::SaveCache(m_report);
    }

    if (!CreateDialogWindow())
    {
        return;
    }

    RunModalLoop(m_hOwner);
}

// ============================================================================
// 建窗
// ============================================================================

bool ContextMenuSettingsDialog::CreateDialogWindow()
{
    if (!EnsureClassRegistered())
    {
        return false;
    }

    // 通用控件（TreeView 需要）
    INITCOMMONCONTROLSEX cc = {};
    cc.dwSize = sizeof(cc);
    cc.dwICC  = ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES;
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

    RECT ownerRect = {};
    if (m_hOwner != nullptr)
    {
        ::GetWindowRect(m_hOwner, &ownerRect);
    }
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top  + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    m_hWnd = ::CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                               kClassName,
                               L"配置右键菜单",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               x, y, width, height,
                               m_hOwner,
                               nullptr,
                               ::GetModuleHandleW(nullptr),
                               this);

    if (m_hWnd == nullptr)
    {
        Logger::Error(L"配置右键菜单窗口创建失败");
        return false;
    }

    CreateControls();
    Layout();
    ReloadTree();
    return true;
}

void ContextMenuSettingsDialog::CreateControls()
{
    // 顶部状态标签
    m_hStatus = ::CreateWindowExW(0, WC_STATICW, L"",
                                  WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS,
                                  0, 0, 0, 0,
                                  m_hWnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlIdStatus)),
                                  ::GetModuleHandleW(nullptr), nullptr);

    // 刷新按钮
    m_hRefresh = ::CreateWindowExW(0, WC_BUTTONW, L"刷新",
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   0, 0, 0, 0,
                                   m_hWnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlIdRefresh)),
                                   ::GetModuleHandleW(nullptr), nullptr);

    // 中部树形列表
    m_hTree = ::CreateWindowExW(0, WC_TREEVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                     TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT |
                                     TVS_SHOWSELALWAYS,
                                 0, 0, 0, 0,
                                 m_hWnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlIdTree)),
                                 ::GetModuleHandleW(nullptr), nullptr);

    for (HWND ctrl : { m_hStatus, m_hRefresh, m_hTree })
    {
        if (ctrl != nullptr)
        {
            ::SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        }
    }
}

void ContextMenuSettingsDialog::Layout()
{
    if (m_hWnd == nullptr)
    {
        return;
    }

    RECT client = {};
    ::GetClientRect(m_hWnd, &client);

    const int padding = Scale(kPadding);
    const int buttonW = Scale(kButtonWidth);
    const int buttonH = Scale(kButtonHeight);
    const int topH    = Scale(kTopRowHeight);

    // 顶部行：状态标签吃左侧剩余宽度，刷新按钮贴右
    ::SetWindowPos(m_hRefresh, nullptr,
                   client.right - padding - buttonW,
                   padding,
                   buttonW, buttonH, SWP_NOZORDER);

    const int statusRight = client.right - padding - buttonW - Scale(10);
    ::SetWindowPos(m_hStatus, nullptr,
                   padding, padding + (buttonH - Scale(16)) / 2,
                   statusRight - padding, Scale(16), SWP_NOZORDER);

    // 树占满顶部行以下的区域
    ::SetWindowPos(m_hTree, nullptr,
                   padding, padding + topH,
                   client.right - padding * 2,
                   client.bottom - padding * 2 - topH,
                   SWP_NOZORDER);
}

// ============================================================================
// 数据展示
// ============================================================================

void ContextMenuSettingsDialog::ReloadTree()
{
    if (m_hTree == nullptr)
    {
        return;
    }

    ::SendMessageW(m_hTree, TVM_DELETEITEM, 0, reinterpret_cast<LPARAM>(TVI_ROOT));

    // 状态文字
    std::wstring status;
    if (m_report.collectedAt.empty())
    {
        status = L"暂无缓存，点「刷新」从系统采集";
    }
    else
    {
        status = L"最近采集：" + m_report.collectedAt;
    }
    ::SetWindowTextW(m_hStatus, status.c_str());

    // 一级文字 → 节点句柄（二级项按 parentPath 挂回去）
    std::unordered_map<std::wstring, HTREEITEM> firstLevel;

    for (const shell::ContextMenuGroup& group : m_report.groups)
    {
        // 根节点：分组名 + 条目数
        std::wstring rootText = group.name.empty() ? L"（未命名）" : group.name;
        rootText += L"  （" + std::to_wstring(group.items.size()) + L" 项）";
        const HTREEITEM hRoot = InsertNode(m_hTree, TVI_ROOT, rootText, 0);

        for (const shell::ContextMenuItem& item : group.items)
        {
            if (item.isSeparator)
            {
                InsertNode(m_hTree, hRoot, L"—", 0);
                continue;
            }

            // 条目文字：真实菜单文字 + 场景标注；失败项附原因
            std::wstring text = item.text.empty() ? L"（无名）" : item.text;
            text += L"  [" + shell::ScenesToText(item.scenes) + L"]";
            if (item.disabled)
            {
                text += L" （灰态）";
            }
            if (item.failed)
            {
                text += L" （读取失败：" + item.failReason + L"）";
            }

            // 一级项挂根节点；二级项按 parentPath 挂到对应一级节点
            HTREEITEM parent = hRoot;
            if (!item.parentPath.empty())
            {
                const auto it = firstLevel.find(item.parentPath);
                if (it != firstLevel.end())
                {
                    parent = it->second;
                }
            }

            const HTREEITEM hNode = InsertNode(m_hTree, parent, text, 0);

            if (item.parentPath.empty())
            {
                firstLevel[item.text] = hNode;
            }
        }

        // 展开分组根节点
        ::SendMessageW(m_hTree, TVM_EXPAND, TVE_EXPAND,
                       reinterpret_cast<LPARAM>(hRoot));
    }
}

void ContextMenuSettingsDialog::Refresh()
{
    // 重新扫注册表 + 运行时采集 + 落缓存；一期纯查看，不改任何系统设置
    m_report = shell::EnumerateRegistry();
    shell::CollectDynamicItems(m_report);
    shell::SaveCache(m_report);
    ReloadTree();
}

// ============================================================================
// 消息处理
// ============================================================================

LRESULT ContextMenuSettingsDialog::HandleMessage(HWND, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SIZE:
        Layout();
        return 0;

    case WM_COMMAND:
    {
        const int controlId = LOWORD(wParam);
        if (controlId == kControlIdRefresh)
        {
            Refresh();
            return 0;
        }
        break;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            ::DestroyWindow(m_hWnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        ::DestroyWindow(m_hWnd);
        return 0;

    case WM_DESTROY:
        m_hWnd = nullptr;
        return 0;
    }

    return ::DefWindowProcW(m_hWnd, msg, wParam, lParam);
}

} // namespace ui
