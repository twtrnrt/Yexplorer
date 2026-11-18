// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/NavigationPane.cpp
//
// 左侧导航栏（简化版）实现。
// ============================================================================

#include "NavigationPane.h"
#include "core/Logger.h"
#include "shell/ShellFolder.h"   // shell::IsFileSystemPath（右键判断节点有没有真实路径）

#include <ShlObj.h>    // SHGetKnownFolderPath / FOLDERID_*
#include <uxtheme.h>   // SetWindowTheme
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM（右键菜单取坐标用）
#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uxtheme.lib")

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

    // 图标：先挂系统小图标图像列表，把树建出来（图标索引按系统列表算），
    // 建完再换成"左边留白"的自建列表（索引一一对应，不会错位）
    if (iconCache != nullptr && iconCache->ImageList() != nullptr)
    {
        m_hImageList = iconCache->ImageList();
        TreeView_SetImageList(m_hWnd, m_hImageList, TVSIL_NORMAL);
    }

    ApplyFont();

    // 每个导航项目的高度：默认行高偏挤，撑到 30 像素（按当前 DPI 换算）
    ::SendMessageW(m_hWnd, TVM_SETITEMHEIGHT,
                   static_cast<WPARAM>(::MulDiv(30, ::GetDpiForWindow(m_hWnd), 96)), 0);

    BuildTree();

    // 换成资源管理器自己的主题：它自带"图标离左边有点距离"的内边距，
    // 比自己造图像列表干净，也不会动到选中高亮（高亮仍由控件画满整行）
    ::SetWindowTheme(m_hWnd, L"Explorer", nullptr);

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

    // 文件夹图标：用系统文件夹图标
    // 注意不要用 "C:\" 取图标——那会被识别成驱动器，拿到硬盘图标
    int iconFolder = (m_iconCache != nullptr) ? m_iconCache->FolderIconIndex() : 0;

    struct KnownFolder
    {
        const GUID* folderId;
        const wchar_t* displayName;
    };

    const KnownFolder knownFolders[] =
    {
        { &FOLDERID_Desktop,   L"桌面" },
        { &FOLDERID_Downloads, L"下载" },
        { &FOLDERID_Documents, L"文档" },
        { &FOLDERID_Pictures,  L"图片" },
        { &FOLDERID_Music,     L"音乐" },
    };

    // ---------------- 常用位置（最上面这一组）----------------
    // 不再套「快速访问」这层分组标题：那层标题点不了、也不接受拖放，纯占位置，
    // 所以直接把这几项排到最上层，点哪一项就跳哪一项
    for (const auto& known : knownFolders)
    {
        std::wstring path;
        if (!GetKnownFolderPath(*known.folderId, path) || path.empty())
        {
            continue;   // 这台机器没有这个文件夹就跳过
        }

        int iconIndex = (m_iconCache != nullptr)
                        ? m_iconCache->GetIconIndexByPath(path) : iconFolder;
        InsertLeaf(TVI_ROOT, known.displayName, path, iconIndex);
    }

    // ---------------- 分隔线 ----------------
    // 常用位置和下面那组硬盘之间画一条横线（左右各留 10 像素）
    InsertSeparator(TVI_ROOT);

    // ---------------- 各硬盘 ----------------
    // 同样不套「此电脑」这层分组标题，直接用一条横线隔开就够了
    AddDrives(TVI_ROOT);

    // ---------------- 固定区（用户自己拖进来的文件夹 / 程序）----------------
    // 先来一条横线，把固定区和上面的硬盘隔开
    InsertSeparator(TVI_ROOT);

    // 记下固定区的起始位置（就是刚插进去那条分隔线的行顶）。
    // 拖放时靠它判断"鼠标是不是进到固定区了"——进了就只做固定，不移动文件。
    {
        HTREEITEM separatorItem = m_nodes.back().item;
        RECT      separatorRect = {};

        if (separatorItem != nullptr &&
            TreeView_GetItemRect(m_hWnd, separatorItem, &separatorRect, TRUE))
        {
            m_pinnedTop = separatorRect.top;
        }
        else
        {
            m_pinnedTop = -1;
        }
    }

    if (m_pinnedItems.empty())
    {
        // 一条都没有的时候给一行灰字，不然这块一片空白，用户不知道这里能干什么
        InsertHint(TVI_ROOT, L"把文件夹或程序拖到这里");
    }
    else
    {
        for (const core::PinnedItem& item : m_pinnedItems)
        {
            InsertPinned(TVI_ROOT, item);
        }
    }

    Logger::Info(L"左侧导航栏构建完成，节点数=" + std::to_wstring(m_nodes.size()) +
                 L"，固定项=" + std::to_wstring(m_pinnedItems.size()));
}

// 插入一条分隔线用的占位节点
//
// 树控件没有原生的"分隔线"这种东西，惯例做法是插一个空行占位，
// 然后在这一行上自己画一条横线（画在 HandleNotify 的 NM_CUSTOMDRAW 里）。
// 这个节点不是目录：点它不跳转、拖到它上面也不接受。
void NavigationPane::InsertSeparator(HTREEITEM parentItem)
{
    size_t nodeIndex = m_nodes.size();

    NodeData nodeData;
    nodeData.isSeparator = true;      // 标记成"分隔线"，不是目录
    m_nodes.push_back(nodeData);

    TVINSERTSTRUCTW insert = {};
    insert.hParent      = parentItem;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask    = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    insert.item.pszText        = const_cast<wchar_t*>(L"");
    insert.item.lParam         = static_cast<LPARAM>(nodeIndex);
    insert.item.iImage         = I_IMAGENONE;   // 不显示图标
    insert.item.iSelectedImage = I_IMAGENONE;

    HTREEITEM item = TreeView_InsertItem(m_hWnd, &insert);
    m_nodes[nodeIndex].item = item;
}

void NavigationPane::InsertLeaf(HTREEITEM parentItem, const std::wstring& text,
                                const std::wstring& path, int iconIndex)
{
    size_t nodeIndex = m_nodes.size();

    NodeData nodeData;
    nodeData.path = path;
    m_nodes.push_back(nodeData);

    TVINSERTSTRUCTW insert = {};
    insert.hParent      = parentItem;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask    = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    insert.item.pszText        = const_cast<wchar_t*>(text.c_str());
    insert.item.lParam         = static_cast<LPARAM>(nodeIndex);
    insert.item.iImage         = iconIndex;
    insert.item.iSelectedImage = iconIndex;

    HTREEITEM item = TreeView_InsertItem(m_hWnd, &insert);
    m_nodes[nodeIndex].item = item;
}

// 插入一条固定项。
// 文件夹用系统文件夹图标（按路径取，网络盘那种也能取对），
// 程序和快捷方式按完整路径取它自己的图标（和文件列表里看到的一致）。
void NavigationPane::InsertPinned(HTREEITEM parentItem, const core::PinnedItem& item)
{
    int iconIndex = 0;

    if (m_iconCache != nullptr)
    {
        iconIndex = item.isFolder
                    ? m_iconCache->GetIconIndexByPath(item.path)
                    : m_iconCache->GetIconIndexByFullPath(item.path);
    }

    size_t nodeIndex = m_nodes.size();

    NodeData nodeData;
    nodeData.path         = item.path;
    nodeData.isPinned     = true;
    nodeData.isFolderLike = item.isFolder;   // 程序类固定项：右键时只给「取消固定」
    m_nodes.push_back(nodeData);

    TVINSERTSTRUCTW insert = {};
    insert.hParent      = parentItem;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask    = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    insert.item.pszText        = const_cast<wchar_t*>(item.name.c_str());
    insert.item.lParam         = static_cast<LPARAM>(nodeIndex);
    insert.item.iImage         = iconIndex;
    insert.item.iSelectedImage = iconIndex;

    HTREEITEM treeItem = TreeView_InsertItem(m_hWnd, &insert);
    m_nodes[nodeIndex].item = treeItem;
}

// 插入"固定区还空着"时的灰色提示行。
// 这行不是目录：点它不跳转、也不能接收拖放（拖到这里算"拖到固定区"）。
void NavigationPane::InsertHint(HTREEITEM parentItem, const std::wstring& text)
{
    size_t nodeIndex = m_nodes.size();

    NodeData nodeData;
    nodeData.isHint = true;
    m_nodes.push_back(nodeData);

    TVINSERTSTRUCTW insert = {};
    insert.hParent      = parentItem;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask    = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    insert.item.pszText        = const_cast<wchar_t*>(text.c_str());
    insert.item.lParam         = static_cast<LPARAM>(nodeIndex);
    insert.item.iImage         = I_IMAGENONE;
    insert.item.iSelectedImage = I_IMAGENONE;

    HTREEITEM treeItem = TreeView_InsertItem(m_hWnd, &insert);
    m_nodes[nodeIndex].item = treeItem;
}

// ============================================================================
// 固定区
// ============================================================================

void NavigationPane::SetPinnedItems(const std::vector<core::PinnedItem>& items)
{
    m_pinnedItems = items;

    if (m_hWnd == nullptr)
    {
        return;
    }

    BuildTree();   // 整树重建（项目就十几行，重建比增量改省心）
}

bool NavigationPane::IsInPinnedArea(POINT ptClient) const
{
    return m_pinnedTop >= 0 && ptClient.y >= m_pinnedTop;
}

// ----------------------------------------------------------------------------
// 右键菜单：这里只负责"认准压在哪个节点上"，菜单内容和命令执行都交给 MainWindow
//
// 以前这里自己拼一个只有「取消固定」的小菜单；现在要和文件列表里"文件夹右键"
// 保持一致（打开 / 在新窗口中打开 / 复制文件路径 / 在终端中打开 / 属性，
// 固定项再加「取消固定」），那套逻辑在 MainWindow 里已经有一套，
// 两处各写一遍会越走越偏，所以统一交出去。
//
// 不弹菜单的情况（和以前一样）：
//   · 空白处、分组横线、固定区空着时的提示行；
//   · "此电脑""控制面板"这类虚拟节点（没有真实路径，不具备任何可用动作）。
// ----------------------------------------------------------------------------
LRESULT NavigationPane::ShowContextMenu(int screenX, int screenY)
{
    if (m_hWnd == nullptr)
    {
        return 0;
    }

    POINT pt = { screenX, screenY };

    if (screenX == -1 && screenY == -1)
    {
        // 键盘（Shift+F10 / 菜单键）唤起的：改用当前选中项的位置
        HTREEITEM selected = TreeView_GetSelection(m_hWnd);
        RECT      selectedRect = {};

        if (selected == nullptr || !TreeView_GetItemRect(m_hWnd, selected, &selectedRect, FALSE))
        {
            return 0;
        }

        pt.x = selectedRect.left;
        pt.y = selectedRect.bottom;
    }
    else
    {
        ::ScreenToClient(m_hWnd, &pt);
    }

    // 已经把空白处、分隔线、提示行、展开箭头都排除掉了
    HTREEITEM hitItem = HitTestDroppableItem(pt);
    if (hitItem == nullptr)
    {
        return 0;
    }

    TVITEMW treeItem = {};
    treeItem.mask  = TVIF_PARAM;
    treeItem.hItem = hitItem;

    if (!TreeView_GetItem(m_hWnd, &treeItem))
    {
        return 0;
    }

    size_t nodeIndex = static_cast<size_t>(treeItem.lParam);
    if (nodeIndex >= m_nodes.size())
    {
        return 0;
    }

    const NodeData& node = m_nodes[nodeIndex];

    // 非固定项里只有真实文件系统路径（桌面/下载/文档/硬盘）才弹；
    // 固定项本身可能是程序，也要弹（菜单里只剩「取消固定」）
    if (!node.isPinned && !shell::IsFileSystemPath(node.path))
    {
        return 0;
    }

    // 先把这一项选中，让用户一眼看出操作的是哪个
    TreeView_SelectItem(m_hWnd, hitItem);

    POINT screenPt = pt;
    ::ClientToScreen(m_hWnd, &screenPt);

    if (m_onContextMenu)
    {
        m_onContextMenu(node.path, node.isPinned, node.isFolderLike, screenPt);
    }

    return 0;
}

// 右键菜单不用子类化 WM_CONTEXTMENU，改成走 HandleNotify 里的 NM_RCLICK
// （NM_RCLICK 是树控件给父窗口的标准右键通知，不用额外挂窗口过程，更省事也更稳）。
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
//   · 分隔线那一行不是目录，不能放
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

    if (m_nodes[nodeIndex].isSeparator || m_nodes[nodeIndex].isHint)
    {
        return nullptr;   // 分隔线、提示行都不接受放置
    }

    return hitItem;
}

// 取某个节点对应的目录路径（分隔条、空白返回空串）
std::wstring NavigationPane::ItemPath(HTREEITEM item) const
{
    if (m_hWnd == nullptr || item == nullptr)
    {
        return std::wstring();
    }

    TVITEMW treeItem = {};
    treeItem.mask  = TVIF_PARAM;
    treeItem.hItem = item;

    if (!TreeView_GetItem(m_hWnd, &treeItem))
    {
        return std::wstring();
    }

    size_t nodeIndex = static_cast<size_t>(treeItem.lParam);
    if (nodeIndex >= m_nodes.size())
    {
        return std::wstring();
    }

    return m_nodes[nodeIndex].path;
}

std::wstring NavigationPane::HitTestPath(POINT ptClient) const
{
    return ItemPath(HitTestDroppableItem(ptClient));
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

// ============================================================================
// 左栏选中状态跟着当前目录走
// ============================================================================

// 比较两个路径是不是同一个目录：忽略大小写，忽略末尾多余的反斜杠
static bool SameFolder(const std::wstring& a, const std::wstring& b)
{
    std::wstring x = a;
    std::wstring y = b;

    // "C:\" 这种盘符根长度正好 3，不能再砍，否则变成 "C:" 就比错了
    while (x.size() > 3 && (x.back() == L'\\' || x.back() == L'/'))
    {
        x.pop_back();
    }
    while (y.size() > 3 && (y.back() == L'\\' || y.back() == L'/'))
    {
        y.pop_back();
    }

    return ::_wcsicmp(x.c_str(), y.c_str()) == 0;
}

void NavigationPane::SelectFolderPath(const std::wstring& path)
{
    if (m_hWnd == nullptr)
    {
        return;
    }

    HTREEITEM matched = nullptr;
    HTREEITEM matchedPinned = nullptr;   // 固定项先记下来，上面那组没有命中时才用它

    for (const NodeData& node : m_nodes)
    {
        if (node.isSeparator || node.isHint || node.item == nullptr)
        {
            continue;
        }

        if (!SameFolder(node.path, path))
        {
            continue;
        }

        // 同一个目录可能既有"文档"这种常用项、又被用户固定了一份。
        // 优先点亮上面那组（固定区里那个只是快捷入口，不该抢主选中）。
        if (node.isPinned)
        {
            if (matchedPinned == nullptr)
            {
                matchedPinned = node.item;
            }
            continue;
        }

        matched = node.item;
        break;
    }

    if (matched == nullptr)
    {
        matched = matchedPinned;
    }

    // 找到就选中；找不到（进到了某个硬盘里的子文件夹）就清空选中。
    // TreeView_SelectItem 触发的是 TVC_BYPROGRAM，HandleNotify 里只认
    // TVC_BYMOUSE / TVC_BYKEYBOARD，所以这种"程序性选中"不会引发导航。
    TreeView_SelectItem(m_hWnd, matched);
}

LRESULT NavigationPane::HandleNotify(LPARAM lParam)
{
    if (lParam == 0)
    {
        return 0;
    }

    NMHDR* header = reinterpret_cast<NMHDR*>(lParam);

    // ------------------------------------------------------------
    // 右键：树控件的 NM_RCLICK（固定区那一项上才弹「取消固定」）
    // ------------------------------------------------------------
    if (header->code == NM_RCLICK)
    {
        // 用光标实际位置（GetMessagePos 在某些情况下可能拿到更早一条消息的位置）
        POINT cursor = {};
        ::GetCursorPos(&cursor);
        return ShowContextMenu(cursor.x, cursor.y);
    }

    // ------------------------------------------------------------
    // 分隔线：那条横线是自己画的
    //
    // 树控件没有原生「分隔线」，惯例是插一个空行占位，然后在这一行上画线。
    // 这里只接管「分隔线」这一行的绘制，其它行原样交给控件（不碰它们的观感）。
    // ------------------------------------------------------------
    if (header->code == NM_CUSTOMDRAW)
    {
        NMTVCUSTOMDRAW* customDraw = reinterpret_cast<NMTVCUSTOMDRAW*>(lParam);

        if (customDraw->nmcd.dwDrawStage == CDDS_PREPAINT)
        {
            return CDRF_NOTIFYITEMDRAW;   // 每一行都问我们一次要不要自己画
        }

        if (customDraw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
        {
            size_t nodeIndex = static_cast<size_t>(customDraw->nmcd.lItemlParam);

            if (nodeIndex < m_nodes.size() && m_nodes[nodeIndex].isSeparator)
            {
                RECT rowRect = {};
                if (TreeView_GetItemRect(m_hWnd,
                                         reinterpret_cast<HTREEITEM>(customDraw->nmcd.dwItemSpec),
                                         &rowRect, TRUE))
                {
                    // 这一行是空节点，控件给出的"整行矩形"只有很窄一块，
                    // 不能拿它当线的长度——线要按树控件的整个宽度来画。
                    RECT clientRect = {};
                    ::GetClientRect(m_hWnd, &clientRect);

                    // 先铺满这一行的底色（横跨控件整宽）
                    RECT rowBack = {};
                    rowBack.left   = clientRect.left;
                    rowBack.right  = clientRect.right;
                    rowBack.top    = rowRect.top;
                    rowBack.bottom = rowRect.bottom;
                    ::FillRect(customDraw->nmcd.hdc, &rowBack, ::GetSysColorBrush(COLOR_WINDOW));

                    // 暂时不内缩：线从导航栏最左画到最右，先看对称效果
                    int inset = 0;
                    int lineY = (rowRect.top + rowRect.bottom) / 2;

                    RECT lineRect = {};
                    lineRect.left   = clientRect.left + inset;
                    lineRect.right  = clientRect.right - inset;
                    lineRect.top    = lineY;
                    lineRect.bottom = lineY + 1;

                    ::FillRect(customDraw->nmcd.hdc, &lineRect, ::GetSysColorBrush(COLOR_3DLIGHT));
                }

                return CDRF_SKIPDEFAULT;   // 这一行完全由我们自己画
            }

            // 固定区空着时的提示行：文字画成灰色，看起来是"说明"而不是"能点的东西"
            if (nodeIndex < m_nodes.size() && m_nodes[nodeIndex].isHint)
            {
                customDraw->clrText = ::GetSysColor(COLOR_GRAYTEXT);
                return CDRF_NEWFONT;
            }

            return CDRF_DODEFAULT;
        }

        return CDRF_DODEFAULT;
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

    // 分隔线那一行只是占位，点了不跳转
    if (m_nodes[nodeIndex].isSeparator)
    {
        return 0;
    }

    if (m_onNavigate && !m_nodes[nodeIndex].path.empty())
    {
        m_onNavigate(m_nodes[nodeIndex].path);
    }

    return 0;
}
