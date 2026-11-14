// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/NavigationPane.cpp
//
// 左侧导航栏（简化版）实现。
// ============================================================================

#include "NavigationPane.h"
#include "core/Logger.h"

#include <ShlObj.h>    // SHGetKnownFolderPath / FOLDERID_*
#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace
{

// 「此电脑」的 CLSID 路径。传给 SHGetFileInfoW 可以直接拿到系统的电脑图标。
const wchar_t* kMyComputerClsid = L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}";

} // namespace

// ============================================================================
// 构造 / 析构
// ============================================================================

NavigationPane::~NavigationPane()
{
    if (m_hFont != nullptr)
    {
        ::DeleteObject(m_hFont);
        m_hFont = nullptr;
    }
    // 图像列表属于 ShellIcon 缓存，不在这里释放
}

// ============================================================================
// 创建
// ============================================================================

bool NavigationPane::Create(HWND parent, HINSTANCE hInstance, shell::IconCache* iconCache)
{
    m_iconCache = iconCache;

    // 样式说明：
    //   TVS_HASBUTTONS    分组节点显示展开箭头（和资源管理器一致）
    //   TVS_FULLROWSELECT 整行高亮（和资源管理器一致）
    //   TVS_SHOWSELALWAYS 失去焦点时仍显示选中项
    //   TVS_NOHSCROLL     不要横向滚动条（长名字由控件自己截断）
    //   不设 TVS_HASLINES  ——资源管理器左栏没有连线
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP
                | TVS_HASBUTTONS
                | TVS_FULLROWSELECT
                | TVS_SHOWSELALWAYS
                | TVS_NOHSCROLL;

    m_hWnd = ::CreateWindowExW(0, WC_TREEVIEWW, L"", style,
                               0, 0, 0, 0,
                               parent, nullptr, hInstance, nullptr);

    if (m_hWnd == nullptr)
    {
        return false;
    }

    // 图标：直接借系统小图标图像列表
    if (iconCache != nullptr && iconCache->ImageList() != nullptr)
    {
        m_hImageList = iconCache->ImageList();
        TreeView_SetImageList(m_hWnd, m_hImageList, TVSIL_NORMAL);
    }

    ApplyFont();
    BuildTree();

    return true;
}

void NavigationPane::ApplyFont()
{
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);

    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
    {
        m_hFont = ::CreateFontIndirectW(&metrics.lfMessageFont);
    }

    if (m_hFont == nullptr)
    {
        m_hFont = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    }

    if (m_hFont != nullptr)
    {
        ::SendMessageW(m_hWnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFont), TRUE);
    }
}

void NavigationPane::Resize(int x, int y, int width, int height)
{
    if (m_hWnd == nullptr)
    {
        return;
    }
    ::SetWindowPos(m_hWnd, nullptr, x, y, width, height,
                   SWP_NOZORDER | SWP_NOACTIVATE);
}

// ============================================================================
// 构建树
// ============================================================================

void NavigationPane::BuildTree()
{
    TreeView_DeleteAllItems(m_hWnd);
    m_nodes.clear();

    // 「快速访问」分组标题用一个普通文件夹图标
    // 注意不要用 "C:\" 取图标——那会被识别成驱动器，拿到硬盘图标
    int iconFolder   = (m_iconCache != nullptr) ? m_iconCache->FolderIconIndex() : 0;
    int iconComputer = (m_iconCache != nullptr)
                       ? m_iconCache->GetIconIndexByPath(kMyComputerClsid) : iconFolder;

    // ---------------- 快速访问 ----------------
    HTREEITEM quickAccessItem = nullptr;
    InsertGroup(TVI_ROOT, L"快速访问", iconFolder);
    {
        // 刚插入的分组就是 TreeView 的最后一个根节点，取回来当父节点
        quickAccessItem = TreeView_GetRoot(m_hWnd);
        while (TreeView_GetNextSibling(m_hWnd, quickAccessItem) != nullptr)
        {
            quickAccessItem = TreeView_GetNextSibling(m_hWnd, quickAccessItem);
        }
    }

    struct KnownFolder
    {
        const GUID* folderId;
        const wchar_t* displayName;
    };

    const KnownFolder knownFolders[] =
    {
        { &FOLDERID_Desktop,  L"桌面" },
        { &FOLDERID_Downloads, L"下载" },
        { &FOLDERID_Documents, L"文档" },
        { &FOLDERID_Pictures,  L"图片" },
        { &FOLDERID_Music,     L"音乐" },
    };

    for (const auto& known : knownFolders)
    {
        std::wstring path;
        if (!GetKnownFolderPath(*known.folderId, path) || path.empty())
        {
            continue;   // 这台机器没有这个文件夹就跳过
        }

        int iconIndex = (m_iconCache != nullptr)
                        ? m_iconCache->GetIconIndexByPath(path) : iconFolder;
        InsertLeaf(quickAccessItem, known.displayName, path, iconIndex);
    }

    TreeView_Expand(m_hWnd, quickAccessItem, TVE_EXPAND);

    // ---------------- 此电脑 ----------------
    InsertGroup(TVI_ROOT, L"此电脑", iconComputer);
    HTREEITEM computerItem = TreeView_GetRoot(m_hWnd);
    while (TreeView_GetNextSibling(m_hWnd, computerItem) != nullptr)
    {
        computerItem = TreeView_GetNextSibling(m_hWnd, computerItem);
    }

    AddDrives(computerItem);
    TreeView_Expand(m_hWnd, computerItem, TVE_EXPAND);

    Logger::Info(L"左侧导航栏构建完成，节点数=" + std::to_wstring(m_nodes.size()));
}

void NavigationPane::InsertGroup(HTREEITEM parentItem, const std::wstring& text, int iconIndex)
{
    size_t nodeIndex = m_nodes.size();
    m_nodes.push_back(NodeData{ L"", true });

    TVINSERTSTRUCTW insert = {};
    insert.hParent      = parentItem;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask    = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    insert.item.pszText        = const_cast<wchar_t*>(text.c_str());
    insert.item.lParam         = static_cast<LPARAM>(nodeIndex);
    insert.item.iImage         = iconIndex;
    insert.item.iSelectedImage = iconIndex;

    TreeView_InsertItem(m_hWnd, &insert);
}

void NavigationPane::InsertLeaf(HTREEITEM parentItem, const std::wstring& text,
                                const std::wstring& path, int iconIndex)
{
    size_t nodeIndex = m_nodes.size();
    m_nodes.push_back(NodeData{ path, false });

    TVINSERTSTRUCTW insert = {};
    insert.hParent      = parentItem;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask    = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    insert.item.pszText        = const_cast<wchar_t*>(text.c_str());
    insert.item.lParam         = static_cast<LPARAM>(nodeIndex);
    insert.item.iImage         = iconIndex;
    insert.item.iSelectedImage = iconIndex;

    TreeView_InsertItem(m_hWnd, &insert);
}

void NavigationPane::AddDrives(HTREEITEM parentItem)
{
    DWORD driveMask = ::GetLogicalDrives();

    for (int driveIndex = 0; driveIndex < 26; ++driveIndex)
    {
        if ((driveMask & (1u << driveIndex)) == 0)
        {
            continue;
        }

        // 拼出根路径，例如 "C:\"
        wchar_t rootPath[4] = {
            static_cast<wchar_t>(L'A' + driveIndex), L':', L'\\', L'\0'
        };

        UINT driveType = ::GetDriveTypeW(rootPath);
        if (driveType == DRIVE_NO_ROOT_DIR || driveType == DRIVE_UNKNOWN)
        {
            continue;
        }

        // 一次查询同时拿「显示名」（如「本地磁盘 (C:)」）和图标
        SHFILEINFOW fileInfo = {};
        DWORD_PTR result = ::SHGetFileInfoW(
            rootPath,
            0,
            &fileInfo,
            sizeof(fileInfo),
            SHGFI_DISPLAYNAME | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);

        std::wstring displayName;
        int iconIndex = 0;

        if (result != 0 && fileInfo.szDisplayName[0] != L'\0')
        {
            displayName = fileInfo.szDisplayName;
            iconIndex   = fileInfo.iIcon;
        }
        else
        {
            // 光驱没放盘片之类的情况，用盘符当名字
            displayName = std::wstring(rootPath, 2);
        }

        InsertLeaf(parentItem, displayName, rootPath, iconIndex);
    }
}

bool NavigationPane::GetKnownFolderPath(const GUID& folderId, std::wstring& outPath)
{
    PWSTR rawPath = nullptr;
    HRESULT hr = ::SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &rawPath);

    if (FAILED(hr) || rawPath == nullptr)
    {
        if (rawPath != nullptr)
        {
            ::CoTaskMemFree(rawPath);
        }
        return false;
    }

    outPath = rawPath;
    ::CoTaskMemFree(rawPath);
    return true;
}

// ============================================================================
// 通知处理
// ============================================================================

// 拖放用：这个坐标落在哪个「可以放东西」的节点上
//
// 判定规则（和资源管理器左栏一致）：
//   · 整行都算命中（控件开了 TVS_FULLROWSELECT，压在右边空白处也认）
//   · 只有「纯展开箭头」不算（那是展开用的，不是放置区）
//   · 分组标题（快速访问 / 此电脑）只是分类，不是真实目录，也不能放
HTREEITEM NavigationPane::HitTestDroppableItem(POINT ptClient) const
{
    if (m_hWnd == nullptr)
    {
        return nullptr;
    }

    TVHITTESTINFO hitTest = {};
    hitTest.pt = ptClient;
    HTREEITEM hitItem = TreeView_HitTest(m_hWnd, &hitTest);

    if (hitItem == nullptr)
    {
        return nullptr;   // 落在空白处
    }

    if ((hitTest.flags & TVHT_ONITEMBUTTON) != 0 &&
        (hitTest.flags & (TVHT_ONITEMLABEL | TVHT_ONITEMICON | TVHT_ONITEMRIGHT)) == 0)
    {
        return nullptr;   // 只压在展开箭头上
    }

    TVITEMW item = {};
    item.mask  = TVIF_PARAM;
    item.hItem = hitItem;

    if (!TreeView_GetItem(m_hWnd, &item))
    {
        return nullptr;
    }

    size_t nodeIndex = static_cast<size_t>(item.lParam);
    if (nodeIndex >= m_nodes.size())
    {
        return nullptr;
    }

    if (m_nodes[nodeIndex].isGroup)
    {
        return nullptr;   // 分组标题不接受放置
    }

    return hitItem;
}

std::wstring NavigationPane::HitTestPath(POINT ptClient) const
{
    HTREEITEM hitItem = HitTestDroppableItem(ptClient);
    if (hitItem == nullptr)
    {
        return std::wstring();
    }

    TVITEMW item = {};
    item.mask  = TVIF_PARAM;
    item.hItem = hitItem;

    if (!TreeView_GetItem(m_hWnd, &item))
    {
        return std::wstring();
    }

    size_t nodeIndex = static_cast<size_t>(item.lParam);
    if (nodeIndex >= m_nodes.size())
    {
        return std::wstring();
    }

    return m_nodes[nodeIndex].path;
}

// 拖放高亮：把鼠标正压着的那一行点亮（用系统自带的 TVIS_DROPHILITED）
//
// 为什么用系统的状态而不是自己画：
//   自己画要接管 TVN_CUSTOMDRAW / WM_PAINT，还得处理滚动、展开、失焦，
//   系统这个状态本来就是给拖放用的，画出来和资源管理器一模一样，一步到位。
void NavigationPane::SetDropHighlight(HTREEITEM item)
{
    if (m_hWnd == nullptr)
    {
        return;
    }

    // 拖动时 DragOver 会一直调过来，节点没变就立刻返回（不清也不设，省事省时间）
    if (m_dropHighlightItem == item)
    {
        return;
    }

    // 先清掉上一个
    if (m_dropHighlightItem != nullptr)
    {
        TVITEMW clearItem = {};
        clearItem.mask      = TVIF_STATE;
        clearItem.hItem     = m_dropHighlightItem;
        clearItem.state     = 0;
        clearItem.stateMask = TVIS_DROPHILITED;

        ::SendMessageW(m_hWnd, TVM_SETITEM, 0, reinterpret_cast<LPARAM>(&clearItem));
    }

    m_dropHighlightItem = item;

    // 再点亮新的
    if (item != nullptr)
    {
        TVITEMW setItem = {};
        setItem.mask      = TVIF_STATE;
        setItem.hItem     = item;
        setItem.state     = TVIS_DROPHILITED;
        setItem.stateMask = TVIS_DROPHILITED;

        ::SendMessageW(m_hWnd, TVM_SETITEM, 0, reinterpret_cast<LPARAM>(&setItem));
    }
}

LRESULT NavigationPane::HandleNotify(LPARAM lParam)
{
    if (lParam == 0)
    {
        return 0;
    }

    NMTREEVIEWW* treeView = reinterpret_cast<NMTREEVIEWW*>(lParam);

    if (treeView->hdr.code != TVN_SELCHANGEDW)
    {
        return 0;
    }

    // 只有用户真的点了/用键盘选了才跳转，程序性选中不触发
    if (treeView->action != TVC_BYMOUSE && treeView->action != TVC_BYKEYBOARD)
    {
        return 0;
    }

    size_t nodeIndex = static_cast<size_t>(treeView->itemNew.lParam);

    if (nodeIndex >= m_nodes.size())
    {
        return 0;
    }

    // 分组标题（快速访问 / 此电脑）只用来分类，点了不跳转
    if (m_nodes[nodeIndex].isGroup)
    {
        return 0;
    }

    if (m_onNavigate && !m_nodes[nodeIndex].path.empty())
    {
        m_onNavigate(m_nodes[nodeIndex].path);
    }

    return 0;
}
