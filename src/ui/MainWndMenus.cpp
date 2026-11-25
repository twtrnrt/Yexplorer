// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/MainWndMenus.cpp
//
// 右键菜单的构建与弹出（MainWindow 的成员函数，实现从 MainWnd.cpp 拆出来）。
//
// 【为什么单独一个翻译单元】
//   这块约 660 行：按场景拼菜单、左栏右键菜单、自绘菜单项的绘制。
//   它只用 MainWindow 的成员状态（选中项、场景标记、撤销栈、账本），
//   不引入新依赖，所以整体搬到独立 .cpp 最省事、行为零变化。
//   真要解耦成 ContextMenuBuilder 时，再从这里往外抽一次即可。
// ============================================================================

#include "MainWnd.h"

#include "core/Logger.h"
#include "core/Settings.h"
#include "shell/ActionRunner.h"
#include "shell/ShellClipboard.h"
#include "ui/MenuLayout.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <vector>

#pragma comment(lib, "shlwapi.lib")

// ============================================================================
// 右键菜单
//
// 资源管理器的右键菜单是按"点在什么上"重新组合的，不是一套菜单加灰。
// 这里同样分场景：
//
//   Background    文件夹空白处：查看 / 粘贴 / 新建文件夹 / 新建 / 刷新 / 属性
//   SingleFile    单选一个文件：打开 / 打开方式 / 剪切 / 复制 / 删除 / 重命名 / 属性
//   SingleFolder  单选一个文件夹：打开 / 在新窗口中打开 / 剪切 / 复制 / 删除 / 重命名 / 属性
//   Multiple      选中多个：打开 / 剪切 / 复制 / 删除 / 属性
//   ShellItem     盘符 / 此电脑 / 控制面板：打开 / 在新窗口中打开 / 属性
//   None          组标题 / 分隔行：不弹菜单
//
// 场景判定只用已经加载在内存里的条目数据（isFolder / 路径前缀 / 选中数量），
// 不读注册表、不调 COM、不探测文件 —— 右键菜单依然是零成本。
// ============================================================================

void MainWindow::ShowContextMenu(POINT screenPoint)
{
    // 每次弹菜单前把上一次的自绘项数据清掉（下面的 owner-draw 项要用到它们）
    m_menuDrawItems.clear();
    for (HBITMAP bitmap : m_menuBitmaps) ::DeleteObject(bitmap);
    m_menuBitmaps.clear();
    m_liveSubMenus.clear();
    m_runtimeMenuEntries.clear();
    m_menuPrimaryIsContainerFile = false;

    // 上一次的自定义动作命令已经执行完（WM_COMMAND 是菜单关闭后才到），
    // 这里开始新的一轮，旧的运行条目可以丢了。
    m_customActionEntries.clear();

    // 屏蔽「剪切 / 删除」的标记（「主文件夹」里的桌面、文档这些系统入口用），
    // 每次弹菜单先清掉，下面判定场景时再按需要打开
    m_menuSuppressDeleteCut = false;

    // 这次右键是"按住 Shift 点的"吗？
    // 是的话「删除」执行永久删除（不进回收站），菜单文字保持和资源管理器一样（仍是「删除」），
    // 只是点下去之后系统会弹"确实要永久删除"的确认框。
    m_menuPermanentDelete = ((::GetKeyState(VK_SHIFT) & 0x8000) != 0);

    const std::vector<std::wstring> selected = m_fileListView.GetSelectedPaths();

    // ---- 1. 判定场景 ----
    ContextScene scene         = ContextScene::Background;
    std::wstring primaryPath;       // 单选时的那一项
    bool         primaryIsFolder = false;

    if (!selected.empty())
    {
        primaryPath = selected.front();

        const bool isShellNamespace = (primaryPath.compare(0, 2, L"::") == 0) ||
                                      (primaryPath.compare(0, 6, L"shell:") == 0);

        // 条目的元数据（是不是文件夹、是不是组标题）都从当前枚举结果里取
        std::shared_ptr<shell::EnumerateResult> result = CurrentResult();

        bool isGroupHeader = false;
        bool isFolder      = false;
        bool isContainerFile = false;
        bool isSystemItem  = false;

        if (result != nullptr)
        {
            const int index = ListView_GetNextItem(m_fileListView.Handle(), -1, LVNI_SELECTED);
            if (index >= 0 && index < static_cast<int>(result->entries.size()))
            {
                const shell::ShellEntry& entry = result->entries[index];
                isGroupHeader = entry.isGroupHeader || entry.isSpacer;
                isFolder      = entry.isFolder;
                isContainerFile = entry.isContainerFile;
                isSystemItem  = entry.isSystemItem;
            }
        }

        // 「主文件夹」里的系统入口 —— 桌面、文档、下载、图片、音乐这些 —— 不给「剪切」
        // 和「删除」：它们是 Shell 重定向的已知文件夹（位置记在用户配置里），搬不走也删不得，
        // 系统资源管理器里同样没有这两项。
        //
        // "主文件夹"有两层，都要认：
        //   · ::TopDesktop（本程序对顶层"桌面容器"的叫法）—— 条目带 isSystemItem 标记；
        //   · ::{F874310E-...}（Win11 的 Home，从上面那层再点「主文件夹」进来的）——
        //     条目的路径是真实目录（C:\Users\xxx\Desktop 这种），要按"是不是已知文件夹"认。
        // 其它目录（比如浏览 C:\Users\xxx 时的同名文件夹）不算，照旧给剪切/删除。
        const std::wstring currentPath = m_history.Current();

        if (shell::IsTopDesktopPath(currentPath))
        {
            m_menuSuppressDeleteCut = isSystemItem;
        }
        else if (shell::IsHomeFolderPath(currentPath))
        {
            std::wstring knownFolderName;
            m_menuSuppressDeleteCut = isFolder && shell::KnownFolderDisplayName(primaryPath, knownFolderName);
        }

        primaryIsFolder = isFolder;

        // 记住"选中的是不是压缩包文件"，拼单选文件菜单时要靠它决定
        // 要不要多给一条「在新窗口中打开」（和文件夹一致）
        m_menuPrimaryIsContainerFile = isContainerFile;

        if (isGroupHeader)
        {
            scene = ContextScene::None;            // 组标题、分隔行：不弹菜单
        }
        else if (selected.size() >= 2)
        {
            scene = ContextScene::Multiple;
        }
        else if (isShellNamespace || (isFolder && ::PathIsRootW(primaryPath.c_str())))
        {
            // 盘符、此电脑、控制面板这些：不给删除/重命名（删盘符是灾难）
            scene = ContextScene::ShellItem;
        }
        else if (isFolder)
        {
            scene = ContextScene::SingleFolder;
        }
        else
        {
            scene = ContextScene::SingleFile;
        }
    }

    if (scene == ContextScene::None)
    {
        return;
    }

    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr)
    {
        return;
    }

    // ---- 2. 按场景构建「这次有哪些项可用」，再按用户配置渲染 ----
    switch (scene)
    {
    case ContextScene::Background:
        RenderMenu(menu, scene, BuildBackgroundMenuSpecs());
        break;

    case ContextScene::SingleFile:
        // 压缩包内部保持固定菜单：那里的路径不是真实文件系统项，
        // 不参与自定义（打开 / 复制 两项即可）。
        if (m_currentIsContainer)
        {
            BuildContainerItemMenu(menu);
        }
        else
        {
            RenderMenu(menu, scene, BuildSingleFileMenuSpecs(primaryPath));
        }
        break;

    case ContextScene::SingleFolder:
        if (m_currentIsContainer)
        {
            BuildContainerItemMenu(menu);
        }
        else
        {
            RenderMenu(menu, scene, BuildSingleFolderMenuSpecs(primaryPath));
        }
        break;

    case ContextScene::Multiple:
        if (m_currentIsContainer)
        {
            BuildContainerItemMenu(menu);
        }
        else
        {
            RenderMenu(menu, scene, BuildMultipleMenuSpecs());
        }
        break;

    case ContextScene::ShellItem:
        // 盘符 / 此电脑 / 控制面板：保持固定菜单，不参与自定义
        BuildShellItemMenu(menu, primaryPath);
        break;

    default:
        break;
    }

    // 用户把这一场景的所有项都隐藏了：不弹一个空菜单
    if (::GetMenuItemCount(menu) == 0)
    {
        ::DestroyMenu(menu);
        m_menuDrawItems.clear();
        return;
    }

    // TPM_RETURNCMD：菜单自己跑完并返回点了哪一项，省去额外的一层消息往返
    int command = ::TrackPopupMenu(menu,
                                   TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
                                   screenPoint.x, screenPoint.y,
                                   0, m_hWnd, nullptr);

    ::DestroyMenu(menu);

    // 菜单已经关了，自绘项的数据和挂上去的图标位图都可以放掉
    // （下次弹菜单前还会再清一次，双保险）
    m_menuDrawItems.clear();
    m_liveSubMenus.clear();
    for (HBITMAP bitmap : m_menuBitmaps) ::DeleteObject(bitmap);
    m_menuBitmaps.clear();

    if (command != 0)
    {
        ::PostMessageW(m_hWnd, WM_COMMAND, MAKEWPARAM(static_cast<WORD>(command), 0), 0);
    }
}

// ---------------------------------------------------------------------------
// 「复制 / 剪切」能不能用
//
//   · 驱动器（D:\ 这种盘符根）不能剪切 —— 整盘搬不走，资源管理器里也是灰的；
//   · 控制面板、"此电脑"里的控制项这类虚拟对象（没有真实文件系统路径）
//     既不能复制也不能剪切。
// 压缩包文件本身和普通文件一样：zip/rar 都能剪。
// 这些判断只查字符串，零成本（符合"右键菜单弹出不查注册表、不碰 COM"的要求）。
// ---------------------------------------------------------------------------
static bool CanCopyPath(const std::wstring& path)
{
    return shell::IsFileSystemPath(path);
}

static bool CanCutPath(const std::wstring& path)
{
    return shell::IsFileSystemPath(path) &&
           !shell::IsDriveRootPath(path);
}

// ---------------------------------------------------------------------------
// 菜单项描述构建
//
// 这一层只回答「这次右键有哪些项、可用不可用」，不管顺序和显隐。
// 顺序 / 显隐在 RenderMenu 里按 MenuLayout 处理。
// ---------------------------------------------------------------------------

namespace
{

ui::MenuSpec MakeItem(const wchar_t* id, const wchar_t* text, UINT command,
                      UINT flags = MF_STRING, bool available = true)
{
    ui::MenuSpec spec;
    spec.id        = id;
    spec.text      = text;
    spec.command   = command;
    spec.flags     = flags;
    spec.available = available;
    return spec;
}

ui::MenuSpec MakeSubmenu(const wchar_t* id, const wchar_t* text,
                         std::function<void(HMENU)> fill, bool available = true)
{
    ui::MenuSpec spec;
    spec.id        = id;
    spec.text      = text;
    spec.available = available;
    spec.fillSubmenu = std::move(fill);
    return spec;
}

// 「撤销 / 恢复」的文案和可用状态收在一处，和改造前 FillUndoItem 完全一致。
ui::MenuSpec MakeUndoRedoSpec(bool redo, const core::UndoRecord* top, UINT command)
{
    ui::MenuSpec spec;
    spec.id      = redo ? L"redo" : L"undo";
    spec.command = command;

    const wchar_t* verb = nullptr;
    if (top != nullptr)
    {
        verb = top->batchRename ? L"批量重命名" : core::UndoStack::VerbText(top->kind);
    }

    spec.available = (verb != nullptr);
    if (verb != nullptr)
    {
        spec.text = std::wstring(redo ? L"恢复 " : L"撤销 ") + verb +
                    (redo ? L"(&E)" : L"(&U)");
    }
    else
    {
        spec.text = redo ? L"恢复(&E)" : L"撤销(&U)";
    }
    return spec;
}

// 去掉多余的分隔线：首尾各一条不要，连续两条只留一条。
// 用户隐藏菜单项之后，原来位置上的分隔线可能悬空，这里统一收尾。
void CleanupMenuSeparators(HMENU menu)
{
    if (menu == nullptr) return;

    int count = ::GetMenuItemCount(menu);
    while (count > 0)
    {
        const UINT state = ::GetMenuState(menu, 0, MF_BYPOSITION);
        if (state == static_cast<UINT>(-1) || (state & MF_SEPARATOR) == 0) break;
        ::DeleteMenu(menu, 0, MF_BYPOSITION);
        --count;
    }

    while (count > 0)
    {
        const UINT state = ::GetMenuState(menu, static_cast<UINT>(count - 1), MF_BYPOSITION);
        if (state == static_cast<UINT>(-1) || (state & MF_SEPARATOR) == 0) break;
        ::DeleteMenu(menu, static_cast<UINT>(count - 1), MF_BYPOSITION);
        --count;
    }

    for (int i = count - 1; i > 0; --i)
    {
        const UINT current  = ::GetMenuState(menu, static_cast<UINT>(i), MF_BYPOSITION);
        const UINT previous = ::GetMenuState(menu, static_cast<UINT>(i - 1), MF_BYPOSITION);
        if (current == static_cast<UINT>(-1) || previous == static_cast<UINT>(-1)) continue;
        if ((current & MF_SEPARATOR) != 0 && (previous & MF_SEPARATOR) != 0)
        {
            ::DeleteMenu(menu, static_cast<UINT>(i), MF_BYPOSITION);
        }
    }
}

// 把 HICON 颜色位图转成菜单项可用的 32 位预乘 alpha DIB。
// 动态组的二级项图标走 MENUITEMINFO + MIIM_BITMAP，不能用 SetMenuItemBitmaps
// （后者是单色勾选图）。这段沿用旧方案已经验证过的 alpha 处理。
HBITMAP CreateMenuBitmapFromIcon(HBITMAP color, int width, int height)
{
    if (color == nullptr || width <= 0 || height <= 0) return nullptr;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = width;
    bmi.bmiHeader.biHeight      = -height;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
    HDC screen = ::CreateCompatibleDC(nullptr);
    if (screen == nullptr) return nullptr;
    const int copied = ::GetDIBits(screen, color, 0, static_cast<UINT>(height),
                                   pixels.data(), &bmi, DIB_RGB_COLORS);
    ::DeleteDC(screen);
    if (copied == 0) return nullptr;

    bool hasNonZeroAlpha = false;
    bool hasNonOpaqueAlpha = false;
    for (size_t i = 3; i < pixels.size(); i += 4)
    {
        const BYTE a = pixels[i];
        if (a != 0) hasNonZeroAlpha = true;
        if (a != 255) hasNonOpaqueAlpha = true;
    }

    if (!hasNonZeroAlpha)
    {
        for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    }
    else if (hasNonOpaqueAlpha)
    {
        for (size_t i = 0; i < pixels.size(); i += 4)
        {
            const BYTE a = pixels[i + 3];
            if (a == 0)
            {
                pixels[i] = pixels[i + 1] = pixels[i + 2] = 0;
            }
            else if (a != 255)
            {
                pixels[i]     = static_cast<BYTE>((pixels[i]     * a + 127) / 255);
                pixels[i + 1] = static_cast<BYTE>((pixels[i + 1] * a + 127) / 255);
                pixels[i + 2] = static_cast<BYTE>((pixels[i + 2] * a + 127) / 255);
            }
        }
    }

    void* bits = nullptr;
    HBITMAP bitmap = ::CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr)
    {
        if (bitmap != nullptr) ::DeleteObject(bitmap);
        return nullptr;
    }
    std::copy(pixels.begin(), pixels.end(), static_cast<BYTE*>(bits));
    return bitmap;
}

} // namespace

void MainWindow::AttachMenuIcon(HMENU menu, UINT position, HICON icon)
{
    if (menu == nullptr || icon == nullptr) return;

    ICONINFO info = {};
    if (!::GetIconInfo(icon, &info)) return;

    HBITMAP color = info.hbmColor;
    HBITMAP mask  = info.hbmMask;
    if (color == nullptr)
    {
        if (mask != nullptr) ::DeleteObject(mask);
        return;
    }

    BITMAP colorInfo = {};
    if (::GetObjectW(color, sizeof(colorInfo), &colorInfo) == 0 ||
        colorInfo.bmBitsPixel < 24)
    {
        ::DeleteObject(color);
        if (mask != nullptr) ::DeleteObject(mask);
        return;
    }

    HBITMAP itemBitmap = CreateMenuBitmapFromIcon(color, colorInfo.bmWidth,
                                                  colorInfo.bmHeight);
    if (itemBitmap != nullptr)
    {
        MENUITEMINFO mii = {};
        mii.cbSize   = sizeof(MENUITEMINFO);
        mii.fMask    = MIIM_BITMAP;
        mii.hbmpItem = itemBitmap;
        if (::SetMenuItemInfoW(menu, position, FALSE, &mii))
        {
            m_menuBitmaps.push_back(itemBitmap);
        }
        else
        {
            ::DeleteObject(itemBitmap);
        }
    }

    ::DeleteObject(color);
    if (mask != nullptr) ::DeleteObject(mask);
}

void MainWindow::AppendThirdPartyGroup(HMENU parent, const shell::AppSourceRow& row,
                                       unsigned int oldScene,
                                       const std::vector<std::wstring>& targetPaths,
                                       const std::wstring& parentFolder)
{
    if (parent == nullptr || row.owners.empty()) return;

    HMENU sub = ::CreatePopupMenu();
    if (sub == nullptr) return;

    ::AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, L"正在读取…");

    LiveSubMenu live;
    live.menu        = sub;
    live.row         = row;
    live.scene       = oldScene;
    live.parentFolder = parentFolder;
    live.targetPaths = targetPaths;
    m_liveSubMenus.push_back(std::move(live));

    // 右键菜单里只显示应用名，不显示（经典）/（现代）
    ::AppendMenuW(parent, MF_POPUP | MF_STRING,
                  reinterpret_cast<UINT_PTR>(sub), row.appName.c_str());

    if (core::Settings::Instance().MenuIcons())
    {
        HICON icon = m_menuIcons.GetOwnerIcon(row.owners.front().server, row.appName);
        if (icon != nullptr)
        {
            AttachMenuIcon(parent, static_cast<UINT>(::GetMenuItemCount(parent) - 1), icon);
        }
    }
}

bool MainWindow::HandleInitMenuPopup(HMENU menu)
{
    for (LiveSubMenu& live : m_liveSubMenus)
    {
        if (live.menu != menu) continue;
        if (live.filled) return true;
        live.filled = true;

        while (::GetMenuItemCount(menu) > 0)
        {
            ::RemoveMenu(menu, 0, MF_BYPOSITION);
        }

        const bool showIcons = core::Settings::Instance().MenuIcons();
        int renderedOwners = 0;

        std::function<void(HMENU, const std::vector<shell::MenuInventoryItem>&)> appendItems;
        appendItems = [&](HMENU target, const std::vector<shell::MenuInventoryItem>& rows)
        {
            for (const shell::MenuInventoryItem& item : rows)
            {
                if (item.separator)
                {
                    ::AppendMenuW(target, MF_SEPARATOR, 0, nullptr);
                    continue;
                }

                const std::wstring text = item.text.empty() ? L"（无文字）" : item.text;

                if (!item.children.empty())
                {
                    HMENU sub = ::CreatePopupMenu();
                    if (sub == nullptr) continue;
                    appendItems(sub, item.children);

                    UINT flags = MF_POPUP | MF_STRING;
                    if (item.disabled) flags |= MF_GRAYED;
                    ::AppendMenuW(target, flags, reinterpret_cast<UINT_PTR>(sub), text.c_str());

                    if (showIcons)
                    {
                        HICON icon = m_menuIcons.GetFile(item.iconFile);
                        if (icon != nullptr)
                        {
                            AttachMenuIcon(target,
                                           static_cast<UINT>(::GetMenuItemCount(target) - 1),
                                           icon);
                        }
                    }
                    continue;
                }

                if (static_cast<int>(m_runtimeMenuEntries.size()) >= kRuntimeMenuMax)
                {
                    continue;
                }

                const int commandId = kRuntimeMenuBase +
                                      static_cast<int>(m_runtimeMenuEntries.size());

                RuntimeMenuEntry runtime;
                runtime.commandId  = commandId;
                runtime.item       = item;
                runtime.targetPaths = live.targetPaths;
                runtime.parentFolder = live.parentFolder;
                m_runtimeMenuEntries.push_back(std::move(runtime));

                UINT flags = MF_STRING;
                if (item.disabled) flags |= MF_GRAYED;

                HICON icon = showIcons ? m_menuIcons.GetFile(item.iconFile) : nullptr;
                if (icon != nullptr)
                {
                    auto drawItem = std::make_unique<MenuDrawItem>();
                    drawItem->text = text;
                    drawItem->icon = icon;

                    MenuDrawItem* raw = drawItem.get();
                    m_menuDrawItems.push_back(std::move(drawItem));
                    ::AppendMenuW(target, flags | MF_OWNERDRAW, commandId,
                                  reinterpret_cast<LPCTSTR>(raw));
                }
                else
                {
                    ::AppendMenuW(target, flags, static_cast<UINT>(commandId), text.c_str());
                }
            }
        };

        for (const shell::DynamicOwner& owner : live.row.owners)
        {
            std::vector<shell::MenuInventoryItem> items;
            std::wstring error;
            const bool ok = shell::CaptureOwnerMenu(
                owner.kind, owner.clsid, live.targetPaths, live.parentFolder,
                live.scene, items, &error);

            if (!ok || items.empty())
            {
                Logger::Warn(L"第三方菜单现场采集失败/为空：" + live.row.appName +
                             L"；" + owner.displayName + L"；" + error);
                continue;
            }


            if (renderedOwners > 0)
            {
                ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            }
            appendItems(menu, items);
            ++renderedOwners;
        }

        if (renderedOwners == 0)
        {
            ::AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"此扩展名无可用功能");
        }
        return true;
    }
    return false;
}

bool MainWindow::InvokeRuntimeMenuCommand(int commandId)
{
    for (const RuntimeMenuEntry& runtime : m_runtimeMenuEntries)
    {
        if (runtime.commandId != commandId) continue;

        std::wstring workingDir = runtime.parentFolder;
        if (workingDir.empty() && !runtime.targetPaths.empty())
        {
            workingDir = runtime.targetPaths.front();
            wchar_t drive[MAX_PATH] = {};
            wchar_t dir[MAX_PATH] = {};
            wchar_t name[MAX_PATH] = {};
            wchar_t ext[MAX_PATH] = {};
            if (::_wsplitpath_s(workingDir.c_str(), drive, MAX_PATH, dir, MAX_PATH,
                                name, MAX_PATH, ext, MAX_PATH) == 0)
            {
                workingDir = std::wstring(drive) + dir;
                if (!workingDir.empty() && workingDir.back() == L'\\') workingDir.pop_back();
            }
        }

        std::wstring error;
        const bool ok = shell::InvokeInventoryItem(runtime.item, runtime.targetPaths,
                                                   workingDir, m_hWnd, &error);
        if (!ok)
        {
            Logger::Warn(L"第三方右键执行失败：" + runtime.item.text + L"；" + error);
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 文件夹空白处
//
//   查看 / 排序 / 刷新 ┃ 粘贴 ┃ 撤销 / 恢复 ┃ 新建文件夹 / 新建 ┃ 在终端中打开 ┃ 属性
// ---------------------------------------------------------------------------
std::vector<ui::MenuSpec> MainWindow::BuildBackgroundMenuSpecs()
{
    std::vector<ui::MenuSpec> specs;

    specs.push_back(MakeSubmenu(L"view", L"查看(&V)",
        [this](HMENU sub) { FillViewMenuContents(sub); }));

    specs.push_back(MakeSubmenu(L"sort", L"排序(&O)",
        [this](HMENU sub) { FillSortMenuContents(sub); },
        !IsSortDisabledHere()));

    specs.push_back(MakeItem(L"refresh", L"刷新(&R)", kCommandRefresh));

    const std::wstring currentPath = m_history.Current();

    // 压缩包内部（不分格式）不显示「粘贴」：那里根本写不进去
    const bool pasteAllowed = !m_currentIsContainer;
    specs.push_back(MakeItem(L"paste", L"粘贴(&P)", kCommandPaste, MF_STRING,
                             pasteAllowed && shellclip::HasFiles()));

    specs.push_back(MakeUndoRedoSpec(false, m_undo.TopUndo(), kCommandUndo));
    specs.push_back(MakeUndoRedoSpec(true, m_undo.TopRedo(), kCommandRedo));

    // 虚拟文件夹（此电脑、控制面板这类）里不能建文件，置灰（和资源管理器一致）
    const bool canCreate = !currentPath.empty() &&
                           currentPath.compare(0, 2, L"::") != 0 &&
                           currentPath.compare(0, 6, L"shell:") != 0;
    specs.push_back(MakeItem(L"newFolder", L"新建文件夹(&N)", kCommandNewFolder,
                             MF_STRING | (canCreate ? 0u : MF_GRAYED)));

    specs.push_back(MakeSubmenu(L"new", L"新建(&W)",
        [this](HMENU sub) { FillNewMenuContents(sub); }));

    // 「在终端中打开」：只在真实文件系统目录里给
    specs.push_back(MakeItem(L"terminal", L"在终端中打开", kCommandOpenTerminal,
                             MF_STRING,
                             pasteAllowed && shell::IsFileSystemPath(currentPath)));

    specs.push_back(MakeItem(L"properties", L"属性(&S)", kCommandProperties));
    return specs;
}

// ---------------------------------------------------------------------------
// 单选一个文件
//
//   打开 / 打开方式 ┃ 剪切 / 复制 / 复制文件路径 ┃ 撤销 / 恢复 ┃ 删除 ┃ 重命名 ┃ 属性
// ---------------------------------------------------------------------------
std::vector<ui::MenuSpec> MainWindow::BuildSingleFileMenuSpecs(const std::wstring& filePath)
{
    std::vector<ui::MenuSpec> specs;

    specs.push_back(MakeItem(L"open", L"打开(&O)", kCommandOpen));

    // 压缩包文件本身和文件夹一样，也能「在新窗口中打开」（Ctrl+双击同理）
    specs.push_back(MakeItem(L"openInNewWindow", L"在新窗口中打开(&N)",
                             kCommandOpenInNewWindow, MF_STRING,
                             m_menuPrimaryIsContainerFile));

    // 打开方式：只给"普通文件"；菜单内容全部来自内存里的账本，零成本
    const std::wstring extension = shell::ExtensionKeyForPath(filePath);
    const bool hasOpenWith = !extension.empty() && shell::IsExtensionHandled(extension);
    specs.push_back(MakeSubmenu(L"openWith", L"打开方式",
        [this, extension](HMENU sub) { FillOpenWithMenuContents(sub, extension); },
        hasOpenWith));

    specs.push_back(MakeItem(L"cut", L"剪切(&T)", kCommandCut,
                             MF_STRING | (CanCutPath(filePath) ? 0u : MF_GRAYED)));
    specs.push_back(MakeItem(L"copy", L"复制(&C)", kCommandCopy,
                             MF_STRING | (CanCopyPath(filePath) ? 0u : MF_GRAYED)));
    specs.push_back(MakeItem(L"copyPath", L"复制文件路径(&A)", kCommandCopyPath));

    specs.push_back(MakeUndoRedoSpec(false, m_undo.TopUndo(), kCommandUndo));
    specs.push_back(MakeUndoRedoSpec(true, m_undo.TopRedo(), kCommandRedo));

    // 按住 Shift 弹的菜单：「删除」改为永久删除（文字不变，行为变，和资源管理器一致）
    specs.push_back(MakeItem(L"delete", L"删除(&D)",
                             m_menuPermanentDelete ? kCommandDeletePermanent : kCommandDelete));

    specs.push_back(MakeItem(L"rename", L"重命名(&M)", kCommandRename));
    specs.push_back(MakeItem(L"properties", L"属性(&S)", kCommandProperties));
    return specs;
}

// ---------------------------------------------------------------------------
// 单选一个文件夹
//
//   打开 / 在新窗口中打开 ┃ 剪切 / 复制 / 复制文件路径 ┃ 撤销 / 恢复
//   ┃ 删除 ┃ 重命名 ┃ 在终端中打开 ┃ 属性
// ---------------------------------------------------------------------------
std::vector<ui::MenuSpec> MainWindow::BuildSingleFolderMenuSpecs(const std::wstring& folderPath)
{
    std::vector<ui::MenuSpec> specs;

    specs.push_back(MakeItem(L"open", L"打开(&O)", kCommandOpen));
    specs.push_back(MakeItem(L"openInNewWindow", L"在新窗口中打开(&N)",
                             kCommandOpenInNewWindow));

    // 「主文件夹」里的系统入口（桌面 / 文档 / 下载 / 图片 / 音乐…）不给「剪切」和「删除」
    specs.push_back(MakeItem(L"cut", L"剪切(&T)", kCommandCut,
                             MF_STRING | (CanCutPath(folderPath) ? 0u : MF_GRAYED),
                             !m_menuSuppressDeleteCut));
    specs.push_back(MakeItem(L"copy", L"复制(&C)", kCommandCopy,
                             MF_STRING | (CanCopyPath(folderPath) ? 0u : MF_GRAYED)));
    specs.push_back(MakeItem(L"copyPath", L"复制文件路径(&A)", kCommandCopyPath));

    specs.push_back(MakeUndoRedoSpec(false, m_undo.TopUndo(), kCommandUndo));
    specs.push_back(MakeUndoRedoSpec(true, m_undo.TopRedo(), kCommandRedo));

    specs.push_back(MakeItem(L"delete", L"删除(&D)",
                             m_menuPermanentDelete ? kCommandDeletePermanent : kCommandDelete,
                             MF_STRING, !m_menuSuppressDeleteCut));

    specs.push_back(MakeItem(L"rename", L"重命名(&M)", kCommandRename));

    // 「在终端中打开」：文件夹是真实目录，可以直接当终端的工作目录
    specs.push_back(MakeItem(L"terminal", L"在终端中打开", kCommandOpenTerminal,
                             MF_STRING, shell::IsFileSystemPath(folderPath)));

    specs.push_back(MakeItem(L"properties", L"属性(&S)", kCommandProperties));
    return specs;
}

// ---------------------------------------------------------------------------
// 选中多个
//
//   打开 ┃ 剪切 / 复制 / 复制文件路径 ┃ 撤销 / 恢复 ┃ 删除 ┃ 批量重命名 ┃ 属性
// ---------------------------------------------------------------------------
std::vector<ui::MenuSpec> MainWindow::BuildMultipleMenuSpecs()
{
    std::vector<ui::MenuSpec> specs;

    specs.push_back(MakeItem(L"open", L"打开(&O)", kCommandOpen));

    // 多选：只要里面有一项不能复制/剪切（驱动器、控制面板等），整条就灰掉
    const std::vector<std::wstring> selectedPaths = m_fileListView.GetSelectedPaths();
    bool canCut  = !selectedPaths.empty();
    bool canCopy = canCut;

    for (const auto& path : selectedPaths)
    {
        canCut  = canCut  && CanCutPath(path);
        canCopy = canCopy && CanCopyPath(path);
    }

    specs.push_back(MakeItem(L"cut", L"剪切(&T)", kCommandCut,
                             MF_STRING | (canCut ? 0u : MF_GRAYED)));
    specs.push_back(MakeItem(L"copy", L"复制(&C)", kCommandCopy,
                             MF_STRING | (canCopy ? 0u : MF_GRAYED)));
    specs.push_back(MakeItem(L"copyPath", L"复制文件路径(&A)", kCommandCopyPath));

    specs.push_back(MakeUndoRedoSpec(false, m_undo.TopUndo(), kCommandUndo));
    specs.push_back(MakeUndoRedoSpec(true, m_undo.TopRedo(), kCommandRedo));

    specs.push_back(MakeItem(L"delete", L"删除(&D)",
                             m_menuPermanentDelete ? kCommandDeletePermanent : kCommandDelete));

    specs.push_back(MakeItem(L"batchRename", L"批量重命名(&R)", kCommandBatchRename));
    specs.push_back(MakeItem(L"properties", L"属性(&S)", kCommandProperties));
    return specs;
}

// ---------------------------------------------------------------------------
// 压缩包内部的右键菜单（在 zip / rar / 7z 里选中文件时走这里）
//
// 系统实际给的项（用户实测资源管理器）：
//   · zip 内部      : 打开 / 剪切 / 复制 / 删除 / 属性
//   · 其它压缩包内部 : 打开 / 复制
//
// 但"剪切 / 删除 / 属性"要真能干活，必须走容器内项的 PIDL 通道；
// 容器里的项没有真实磁盘路径，路径字符串交给 Shell 解析不出来。
// 这条通道还在做，所以这里先只给「打开 / 复制」——宁可少给，也不给一个点了会出错的项。
// ---------------------------------------------------------------------------
void MainWindow::BuildContainerItemMenu(HMENU menu)
{
    ::AppendMenuW(menu, MF_STRING, kCommandOpen, L"打开(&O)");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandCopy, L"复制(&C)");
}

// ---------------------------------------------------------------------------
// 统一渲染：按配置顺序决定谁出现、谁隐藏
// ---------------------------------------------------------------------------
void MainWindow::RenderMenu(HMENU menu, ContextScene scene,
                            const std::vector<ui::MenuSpec>& specs)
{
    if (menu == nullptr) return;

    ui::MenuScene layoutScene = ui::MenuScene::File;
    switch (scene)
    {
    case ContextScene::Background:   layoutScene = ui::MenuScene::Background; break;
    case ContextScene::SingleFile:   layoutScene = ui::MenuScene::File;       break;
    case ContextScene::SingleFolder: layoutScene = ui::MenuScene::Folder;     break;
    case ContextScene::Multiple:     layoutScene = ui::MenuScene::Multiple;   break;
    default: return;
    }

    std::vector<std::wstring> available;
    available.reserve(specs.size());
    for (const ui::MenuSpec& spec : specs)
    {
        if (spec.available) available.push_back(spec.id);
    }

    const ui::MenuLayout& layout = ui::CachedMenuLayout();

    // 这一次右键的选中项 / 当前目录，给快捷动作匹配和展开用
    shell::ActionContext actionContext;
    actionContext.currentDir = m_history.Current();
    actionContext.paths      = m_fileListView.GetSelectedPaths();
    if (layoutScene == ui::MenuScene::Background)
    {
        actionContext.paths.clear();
    }

    const std::wstring actionPrimaryPath =
        actionContext.paths.empty() ? actionContext.currentDir : actionContext.paths.front();

    std::vector<const ui::CustomAction*> matchingActions;
    for (const ui::CustomAction& action : layout.actions)
    {
        if (action.enabled &&
            shell::ActionMatchesPath(action, layoutScene, actionPrimaryPath, actionContext.paths))
        {
            matchingActions.push_back(&action);
        }
    }

    // 第三方「应用 + 来源」：勾了哪些，就按配置顺序挂哪些动态子菜单
    const std::wstring thirdSceneKey = ui::MenuSceneKey(layoutScene);
    const unsigned thirdSceneBit = 1u << static_cast<unsigned>(layoutScene);
    std::vector<const shell::AppSourceRow*> thirdRows;

    auto thirdIt = layout.thirdParty.find(thirdSceneKey);
    if (thirdIt != layout.thirdParty.end())
    {
        for (const ui::ThirdPartySelection& selection : thirdIt->second)
        {
            const shell::AppSourceRow* app = shell::FindAppSourceRow(selection.key);
            if (app == nullptr) continue;
            if ((app->scenes & thirdSceneBit) == 0) continue;
            thirdRows.push_back(app);
        }
    }

    // 老 MenuScene 位给 CaptureOwnerMenu 用
    unsigned int oldScene = 1;
    switch (layoutScene)
    {
    case ui::MenuScene::Background: oldScene = 8; break;
    case ui::MenuScene::File:       oldScene = 1; break;
    case ui::MenuScene::Folder:     oldScene = 2; break;
    case ui::MenuScene::Multiple:   oldScene = 4; break;
    }

    const bool actionsAvailable = !thirdRows.empty() || !matchingActions.empty();
    const std::vector<std::wstring> order =
        ui::ResolveMenuOrder(layout, layoutScene, available, actionsAvailable);

    const bool showIcons = core::Settings::Instance().MenuIcons();

    for (const std::wstring& token : order)
    {
        if (token == L"sep")
        {
            ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }

        if (token == L"actions")
        {
            for (const shell::AppSourceRow* row : thirdRows)
            {
                AppendThirdPartyGroup(menu, *row, oldScene,
                                      actionContext.paths, actionContext.currentDir);
            }

            if (showIcons)
            {
                std::vector<std::wstring> iconPaths;
                for (const ui::CustomAction* action : matchingActions)
                {
                    if (!action->program.empty()) iconPaths.push_back(action->program);
                }
                m_menuIcons.PrefetchPaths(iconPaths);
            }

            for (const ui::CustomAction* action : matchingActions)
            {
                if (static_cast<int>(m_customActionEntries.size()) >= kCommandCustomActionMax)
                {
                    break;
                }

                const int commandId = kCommandCustomActionBase +
                                      static_cast<int>(m_customActionEntries.size());

                CustomActionRuntime runtime;
                runtime.commandId  = commandId;
                runtime.action     = *action;
                runtime.paths      = actionContext.paths;
                runtime.currentDir = actionContext.currentDir;
                m_customActionEntries.push_back(std::move(runtime));

                const std::wstring text =
                    action->title.empty() ? L"（未命名动作）" : action->title;

                HICON icon = showIcons
                    ? m_menuIcons.Get(MenuIconCache::KeyForPath(action->program))
                    : nullptr;

                if (icon != nullptr)
                {
                    auto drawItem = std::make_unique<MenuDrawItem>();
                    drawItem->text = text;
                    drawItem->icon = icon;

                    MenuDrawItem* raw = drawItem.get();
                    m_menuDrawItems.push_back(std::move(drawItem));

                    ::AppendMenuW(menu, MF_OWNERDRAW, commandId,
                                  reinterpret_cast<LPCTSTR>(raw));
                }
                else
                {
                    ::AppendMenuW(menu, MF_STRING, commandId, text.c_str());
                }
            }
            continue;
        }

        const ui::MenuSpec* spec = nullptr;
        for (const ui::MenuSpec& candidate : specs)
        {
            if (candidate.id == token)
            {
                spec = &candidate;
                break;
            }
        }
        if (spec == nullptr || !spec->available) continue;

        if (spec->IsSubmenu())
        {
            HMENU subMenu = ::CreatePopupMenu();
            if (subMenu == nullptr) continue;

            spec->fillSubmenu(subMenu);
            if (::GetMenuItemCount(subMenu) > 0)
            {
                const UINT flags = MF_POPUP | MF_STRING | (spec->flags & MF_GRAYED);
                ::AppendMenuW(menu, flags,
                              reinterpret_cast<UINT_PTR>(subMenu),
                              spec->text.c_str());
            }
            else
            {
                ::DestroyMenu(subMenu);
            }
            continue;
        }

        if (spec->icon != nullptr && showIcons)
        {
            auto drawItem = std::make_unique<MenuDrawItem>();
            drawItem->text = spec->text;
            drawItem->icon = spec->icon;

            MenuDrawItem* raw = drawItem.get();
            m_menuDrawItems.push_back(std::move(drawItem));

            ::AppendMenuW(menu, MF_OWNERDRAW | (spec->flags & MF_GRAYED),
                          spec->command, reinterpret_cast<LPCTSTR>(raw));
            continue;
        }

        ::AppendMenuW(menu, spec->flags, spec->command, spec->text.c_str());
    }

    CleanupMenuSeparators(menu);
}

// ---------------------------------------------------------------------------
// 盘符 / 此电脑 / 控制面板 这类命名空间项
//
//   打开
//   在新窗口中打开
//   ─────
//   在终端中打开        （只有盘符根这种真实路径才给）
//   ─────
//   属性
// ---------------------------------------------------------------------------
void MainWindow::BuildShellItemMenu(HMENU menu, const std::wstring& itemPath)
{
    ::AppendMenuW(menu, MF_STRING, kCommandOpen, L"打开(&O)");
    ::AppendMenuW(menu, MF_STRING, kCommandOpenInNewWindow, L"在新窗口中打开(&N)");

    // 盘符根（"D:\"）是真实目录，可以当终端的工作目录；
    // "此电脑""控制面板"这类虚拟对象没有真实路径，不给这一项
    if (shell::IsFileSystemPath(itemPath))
    {
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, kCommandOpenTerminal, L"在终端中打开");
    }

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandProperties, L"属性(&S)");
}

// ---------------------------------------------------------------------------
// 左栏（导航栏）右键菜单
//
// 由 NavigationPane::ShowContextMenu 回调进来。动作和文件列表里"文件夹右键"
// 保持同一套，但只保留左栏里真正说得通的项：打开 / 在新窗口中打开 /
// 复制文件路径 / 在终端中打开 / 属性（固定项再加「取消固定」）。
//
// 不给"剪切 / 复制 / 删除 / 重命名"：
//   · 左栏右键的那个文件夹不一定在文件列表里选中，删除/重命名要走列表那套状态，
//     误操作代价也大（右键"桌面"一下就能删掉系统文件夹）；
//   · 系统资源管理器左栏右键本来也没有这些项（用户核对过）。
//
// 命令号在本函数内局部定义：菜单自己 TrackPopupMenu 取返回值直接执行，
// 不走 WM_COMMAND，所以不会和文件列表那套 3xxx 命令撞号。
// ---------------------------------------------------------------------------
void MainWindow::ShowNavigationContextMenu(const std::wstring& path, bool isPinned,
                                           bool isFolder, POINT screenPoint)
{
    if (path.empty())
    {
        return;
    }

    enum NavCommand : int
    {
        kNavOpen          = 1,
        kNavOpenNewWindow = 2,
        kNavCopyPath      = 3,
        kNavTerminal      = 4,
        kNavProperties    = 5,
        kNavUnpin         = 6,
    };

    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr)
    {
        return;
    }

    if (isFolder)
    {
        ::AppendMenuW(menu, MF_STRING, kNavOpen, L"打开");
        ::AppendMenuW(menu, MF_STRING, kNavOpenNewWindow, L"在新窗口中打开");

        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, kNavCopyPath, L"复制文件路径");

        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, kNavTerminal, L"在终端中打开");
    }

    // 属性：文件夹和程序类固定项都给
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kNavProperties, L"属性");

    if (isPinned)
    {
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, kNavUnpin, L"取消固定");
    }

    // 回车默认落到「打开」上；程序类固定项没有「打开」，就不设默认项
    if (isFolder)
    {
        ::SetMenuDefaultItem(menu, kNavOpen, FALSE);
    }

    // 菜单主人必须是顶层窗口：给树控件这种子窗口的话，菜单会立刻收起来、键盘也点不动
    int command = ::TrackPopupMenu(menu,
                                   TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
                                   screenPoint.x, screenPoint.y,
                                   0, m_hWnd, nullptr);

    ::DestroyMenu(menu);

    switch (command)
    {
    case kNavOpen:
        NavigateToPath(path);
        break;

    case kNavOpenNewWindow:
        OpenFolderInNewWindow(path, true);
        break;

    case kNavCopyPath:
        if (!shellclip::WriteText(path, m_hWnd))
        {
            Logger::Warn(L"复制文件路径失败（剪贴板可能被别的程序占用）");
        }
        break;

    case kNavTerminal:
        DoOpenTerminalIn(path);
        break;

    case kNavProperties:
        ShowPropertiesForPath(path);
        break;

    case kNavUnpin:
        UnpinPinnedPath(path);
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// 取消固定：从配置里删掉 → 存盘 → 重建左栏（原先写在 SetUnpinCallback 的 lambda 里，
// 现在左栏右键菜单统一在 MainWindow 里做，就搬到这儿了）
// ---------------------------------------------------------------------------
void MainWindow::UnpinPinnedPath(const std::wstring& path)
{
    if (!core::Settings::Instance().RemovePinned(path))
    {
        return;
    }

    core::Settings::Instance().Save();

    m_navigationPane.SetPinnedItems(core::Settings::Instance().PinnedItems());
    m_navigationPane.SelectFolderPath(m_history.Current());   // 重建树后选中状态要补回来

    Logger::Info(L"取消固定：" + path);
}

// ---------------------------------------------------------------------------
// 「查看」子菜单内容（和资源管理器一样，当前视图打勾）
// 只往传进来的子菜单里填，不负责挂到父菜单；顺序/显隐由 RenderMenu 管。
// ---------------------------------------------------------------------------
void MainWindow::FillViewMenuContents(HMENU subMenu)
{
    if (subMenu == nullptr)
    {
        return;
    }

    const FileListView::ViewMode current = m_fileListView.CurrentViewMode();

    ::AppendMenuW(subMenu, MF_STRING | ((current == FileListView::ViewMode::LargeIcon) ? MF_CHECKED : 0),
                  kCommandViewLarge, L"大图标(&L)");
    ::AppendMenuW(subMenu, MF_STRING | ((current == FileListView::ViewMode::SmallIcon) ? MF_CHECKED : 0),
                  kCommandViewSmall, L"小图标(&S)");
    ::AppendMenuW(subMenu, MF_STRING | ((current == FileListView::ViewMode::List) ? MF_CHECKED : 0),
                  kCommandViewList, L"列表(&T)");
    ::AppendMenuW(subMenu, MF_STRING | ((current == FileListView::ViewMode::Details) ? MF_CHECKED : 0),
                  kCommandViewDetails, L"详细信息(&D)");
    ::AppendMenuW(subMenu, MF_STRING | ((current == FileListView::ViewMode::Tiles) ? MF_CHECKED : 0),
                  kCommandViewTiles, L"平铺(&I)");
}

// ---------------------------------------------------------------------------
// 「排序」子菜单内容
//
//   名称 / 修改日期 / 类型 / 大小   当前排序列打勾
//   ─────
//   升序 / 降序                    当前方向打勾
//
// 「此电脑」是分组视图，排序整体禁用：由 MenuSpec.available 控制，不在这里判断。
// ---------------------------------------------------------------------------
void MainWindow::FillSortMenuContents(HMENU subMenu)
{
    if (subMenu == nullptr)
    {
        return;
    }

    ::AppendMenuW(subMenu, MF_STRING | (m_sortColumn == 0 ? MF_CHECKED : 0),
                  kCommandSortName, L"名称(&N)");
    ::AppendMenuW(subMenu, MF_STRING | (m_sortColumn == 1 ? MF_CHECKED : 0),
                  kCommandSortDate, L"修改日期(&D)");
    ::AppendMenuW(subMenu, MF_STRING | (m_sortColumn == 2 ? MF_CHECKED : 0),
                  kCommandSortType, L"类型(&T)");
    ::AppendMenuW(subMenu, MF_STRING | (m_sortColumn == 3 ? MF_CHECKED : 0),
                  kCommandSortSize, L"大小(&Z)");

    ::AppendMenuW(subMenu, MF_SEPARATOR, 0, nullptr);

    ::AppendMenuW(subMenu, MF_STRING | (m_sortAscending ? MF_CHECKED : 0),
                  kCommandSortAsc, L"升序(&A)");
    ::AppendMenuW(subMenu, MF_STRING | (!m_sortAscending ? MF_CHECKED : 0),
                  kCommandSortDesc, L"降序(&E)");
}

// ----------------------------------------------------------------------------
// 打开方式子菜单 / 新建子菜单
//
// 这两块也是菜单代码（原来在 MainWnd.cpp），一起放在这个翻译单元里。
// 它们只读内存里的账本 / 新建项快照，弹出时零 IO。
// ----------------------------------------------------------------------------

void MainWindow::FillOpenWithMenuContents(HMENU subMenu, const std::wstring& extension)
{
    if (subMenu == nullptr)
    {
        return;
    }

    // 菜单里要不要画图标（设置窗口里那个开关）
    const bool showIcons = core::Settings::Instance().MenuIcons();

    // ---- 这里零 IO：只读配置里已经加载好的账本 ----
    const core::OpenWithEntry* entry = core::Settings::Instance().FindOpenWith(extension);

    int visibleCount = 0;
    bool ledgerHasSelf = false;   // 账本里已经有「本程序」这一项了（下面别重复加）

    if (entry != nullptr)
    {
        for (size_t i = 0; i < entry->items.size(); ++i)
        {
            // 命令 ID 只有 3400~3499 这一百个坑，超了就截断
            if (static_cast<int>(i) > 99)
            {
                break;
            }

            const core::OpenWithItem& item = entry->items[i];

            // 没勾选的（用户在设置窗口里取消过）不进菜单
            if (!item.enabled)
            {
                continue;
            }

            // 已知已经卸载/找不到的，直接消失（不标灰）。
            // alive = -1 表示"还没查过"——这种照常显示，
            // 否则右键菜单就得去探测，那正是用户不允许的。
            if (item.alive == 0)
            {
                continue;
            }

            std::wstring text = item.name;
            if (core::OpenWithKey(item) == entry->defaultKey)
            {
                text += L"（默认）";
            }

            if (item.kind == L"self")
            {
                ledgerHasSelf = true;
            }

            const UINT commandId =
                static_cast<UINT>(kCommandOpenWithBase + static_cast<int>(i));

            if (showIcons)
            {
                // 自绘菜单项：图标从内存缓存里拿（没有就留空，绝不在这里探测）
                auto drawItem = std::make_unique<MenuDrawItem>();
                drawItem->text = text;
                drawItem->icon = m_menuIcons.Get(MenuIconCache::KeyForProgram(item));

                MenuDrawItem* raw = drawItem.get();
                m_menuDrawItems.push_back(std::move(drawItem));

                ::AppendMenuW(subMenu, MF_OWNERDRAW, commandId,
                              reinterpret_cast<LPCTSTR>(raw));
            }
            else
            {
                ::AppendMenuW(subMenu, MF_STRING, commandId, text.c_str());
            }

            ++visibleCount;
        }
    }

    if (visibleCount > 0)
    {
        ::AppendMenuW(subMenu, MF_SEPARATOR, 0, nullptr);
    }

    // 压缩包（zip/rar/7z…）：固定给一条「Yexplorer（本程序）」入口
    //
    // 为什么要固定加：账本里记默认程序时，压缩包的默认很可能是 WinRAR/7-Zip
    // （系统关联就是这样）。用户想用我们自己的文件管理器进去浏览内部，
    // 就得有个随时可点的入口，而不是只能改默认程序。
    // 点它不改账本，只是这一次用本程序打开（进容器浏览）。
    if (shell::IsContainerExtension(extension) && !ledgerHasSelf)
    {
        ::AppendMenuW(subMenu, MF_STRING, kCommandOpenWithSelf,
                      L"Yexplorer");
        ::AppendMenuW(subMenu, MF_SEPARATOR, 0, nullptr);
    }

    // 一个程序都没有时，这一项就是"直接进窗口去设置"
    ::AppendMenuW(subMenu, MF_STRING, kCommandOpenWithManage, L"添加或管理程序…");
}

void MainWindow::FillNewMenuContents(HMENU subMenu)
{
    if (subMenu == nullptr)
    {
        return;
    }

    std::vector<shell::ShellNewItem> items;

    {
        std::lock_guard<std::mutex> lock(m_shellNewMutex);
        items = m_shellNewItems;
    }

    const bool showIcons = core::Settings::Instance().MenuIcons();

    // 虚拟文件夹（此电脑、控制面板这类）里不能建文件，全部变灰（和资源管理器一致）
    const std::wstring currentPath = m_history.Current();
    const bool canCreate = !currentPath.empty() &&
                           currentPath.compare(0, 2, L"::") != 0 &&
                           currentPath.compare(0, 6, L"shell:") != 0;

    for (size_t i = 0; i < items.size() && i < 100; ++i)
    {
        const shell::ShellNewItem& item = items[i];
        const UINT commandId = static_cast<UINT>(kCommandNewBase + static_cast<int>(i));

        if (showIcons)
        {
            auto drawItem = std::make_unique<MenuDrawItem>();
            drawItem->text = item.displayName;
            drawItem->icon = m_menuIcons.Get(MenuIconCache::KeyForNewItem(item));

            MenuDrawItem* raw = drawItem.get();
            m_menuDrawItems.push_back(std::move(drawItem));

            ::AppendMenuW(subMenu, MF_OWNERDRAW | (canCreate ? 0 : MF_GRAYED), commandId,
                          reinterpret_cast<LPCTSTR>(raw));
        }
        else
        {
            ::AppendMenuW(subMenu, MF_STRING | (canCreate ? 0 : MF_GRAYED), commandId,
                          item.displayName.c_str());
        }
    }

    // 快照还没准备好（启动那一刻就点了右键）：至少给个「新建文件夹」占位，
    // 别让菜单是空的
    if (items.empty())
    {
        ::AppendMenuW(subMenu, MF_STRING, kCommandNewFolder, L"文件夹");
    }
}

// ----------------------------------------------------------------------------
// 自绘菜单项绘制
//
// DrawMenuItem 是 WM_DRAWITEM 的实现，用主窗口字体和预取好的小图标画一行。
// 撤销/恢复的菜单项现在由 Build*MenuSpecs 直接生成，不再单独 Fill。
// ----------------------------------------------------------------------------

void MainWindow::DrawMenuItem(const DRAWITEMSTRUCT* drawItem)
{
    if (drawItem == nullptr || drawItem->itemData == 0)
    {
        return;
    }

    const MenuDrawItem* data = reinterpret_cast<const MenuDrawItem*>(drawItem->itemData);
    const RECT& rc = drawItem->rcItem;

    const bool selected = ((drawItem->itemState & ODS_SELECTED) != 0);
    const bool disabled = ((drawItem->itemState & ODS_DISABLED) != 0);

    if (data->modern)
    {
        HBRUSH white = ::CreateSolidBrush(RGB(255, 255, 255));
        ::FillRect(drawItem->hDC, &rc, white);
        ::DeleteObject(white);

        if (selected)
        {
            RECT hover = rc;
            ::InflateRect(&hover, -Scale(4), -Scale(2));

            const COLORREF hoverColor = RGB(240, 242, 245);
            HBRUSH hoverBrush = ::CreateSolidBrush(hoverColor);
            HPEN hoverPen = ::CreatePen(PS_SOLID, 1, hoverColor);
            HGDIOBJ oldBrush = ::SelectObject(drawItem->hDC, hoverBrush);
            HGDIOBJ oldPen = ::SelectObject(drawItem->hDC, hoverPen);
            ::RoundRect(drawItem->hDC, hover.left, hover.top, hover.right, hover.bottom,
                        Scale(6), Scale(6));
            ::SelectObject(drawItem->hDC, oldBrush);
            ::SelectObject(drawItem->hDC, oldPen);
            ::DeleteObject(hoverBrush);
            ::DeleteObject(hoverPen);
        }

        RECT textRect = rc;
        textRect.left += Scale(12);
        textRect.right -= Scale(12);

        ::SetBkMode(drawItem->hDC, TRANSPARENT);
        ::SetTextColor(drawItem->hDC,
                       disabled ? RGB(160, 164, 170) : RGB(42, 46, 52));

        HGDIOBJ oldFont = ::SelectObject(drawItem->hDC, m_hUiFont);
        ::DrawTextW(drawItem->hDC, data->text.c_str(), -1, &textRect,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        ::SelectObject(drawItem->hDC, oldFont);
        return;
    }

    // ---- 底色 ----
    HBRUSH background = ::CreateSolidBrush(
        selected ? ::GetSysColor(COLOR_HIGHLIGHT) : ::GetSysColor(COLOR_MENU));
    ::FillRect(drawItem->hDC, &rc, background);
    ::DeleteObject(background);

    // ---- 图标（左端）----
    const int iconSize = Scale(16);
    const int iconLeft = rc.left + Scale(8);
    const int iconTop  = rc.top + ((rc.bottom - rc.top) - iconSize) / 2;

    if (data->icon != nullptr)
    {
        ::DrawIconEx(drawItem->hDC, iconLeft, iconTop, data->icon, iconSize, iconSize,
                     0, nullptr, DI_NORMAL);
    }

    // ---- 文字 ----
    RECT textRect = rc;
    textRect.left = iconLeft + iconSize + Scale(8);
    textRect.right -= Scale(12);

    ::SetBkMode(drawItem->hDC, TRANSPARENT);
    ::SetTextColor(drawItem->hDC,
                   disabled ? ::GetSysColor(COLOR_GRAYTEXT)
                            : (selected ? ::GetSysColor(COLOR_HIGHLIGHTTEXT)
                                        : ::GetSysColor(COLOR_MENUTEXT)));

    HGDIOBJ oldFont = ::SelectObject(drawItem->hDC, m_hUiFont);
    ::DrawTextW(drawItem->hDC, data->text.c_str(), -1, &textRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    ::SelectObject(drawItem->hDC, oldFont);
}
