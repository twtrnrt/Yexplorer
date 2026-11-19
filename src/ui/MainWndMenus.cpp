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
#include "shell/ShellClipboard.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

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
    m_menuPrimaryIsContainerFile = false;

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

    // ---- 2. 按场景拼菜单 ----
    switch (scene)
    {
    case ContextScene::Background:
        BuildBackgroundMenu(menu);
        break;

    case ContextScene::SingleFile:
        BuildSingleFileMenu(menu, primaryPath);
        break;

    case ContextScene::SingleFolder:
        BuildSingleFolderMenu(menu, primaryPath);
        break;

    case ContextScene::Multiple:
        BuildMultipleMenu(menu);
        break;

    case ContextScene::ShellItem:
        BuildShellItemMenu(menu, primaryPath);
        break;

    default:
        break;
    }

    // TPM_RETURNCMD：菜单自己跑完并返回点了哪一项，省去额外的一层消息往返
    int command = ::TrackPopupMenu(menu,
                                   TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
                                   screenPoint.x, screenPoint.y,
                                   0, m_hWnd, nullptr);

    ::DestroyMenu(menu);

    // 菜单已经关了，自绘项的数据可以放掉（下次弹菜单前还会再清一次，双保险）
    m_menuDrawItems.clear();

    if (command != 0)
    {
        ::PostMessageW(m_hWnd, WM_COMMAND, MAKEWPARAM(static_cast<WORD>(command), 0), 0);
    }
}

// ---------------------------------------------------------------------------
// 文件夹空白处
//
// 分组按用户要求：
//   查看 >
//   刷新            （和「查看」同一组，紧在查看下面）
//   ─────
//   粘贴            （剪贴板里有文件才出现）
//   撤销 / 恢复      （紧跟在「粘贴」那一格的下一格，有才出现）
//   ─────
//   新建文件夹
//   新建 >
//   ─────
//   属性
// ---------------------------------------------------------------------------
void MainWindow::BuildBackgroundMenu(HMENU menu)
{
    FillViewMenu(menu);
    ::AppendMenuW(menu, MF_STRING, kCommandRefresh, L"刷新(&R)");

    // 压缩包内部（不分格式）不显示「粘贴」：
    // 那里根本写不进去，出现了只会让用户点一次、白等一次。
    const bool pasteAllowed = !m_currentIsContainer;

    if (pasteAllowed && shellclip::HasFiles())
    {
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, kCommandPaste, L"粘贴(&P)");
    }

    // 撤销 / 恢复 紧跟在「粘贴」的下一格（自带尾分隔线）
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    FillUndoItem(menu);

    FillNewFolderAndNewMenu(menu);

    // 「在终端中打开」：只在真实文件系统目录里给
    // （压缩包内部是虚拟路径，控制面板那类 "::{GUID}" 也不是真实目录，都没有工作目录可用）
    if (!m_currentIsContainer && shell::IsFileSystemPath(m_history.Current()))
    {
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, kCommandOpenTerminal, L"在终端中打开");
    }

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandProperties, L"属性(&S)");
}

// ---------------------------------------------------------------------------
// 单选一个文件
//
//   打开
//   打开方式 >      （这个扩展名在账本里有程序时才出现）
//   ─────
//   剪切
//   复制
//   复制文件路径        （只把路径文字放进剪贴板，Ctrl+Shift+C 也能触发）
//   ─────
//   撤销 / 恢复      （紧跟「复制」的下一格，有才出现）
//   ─────
//   删除
//   ─────
//   重命名
//   ─────
//   属性
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 「复制 / 剪切」能不能用
//
//   · 驱动器（D:\ 这种盘符根）不能剪切 —— 整盘搬不走，资源管理器里也是灰的；
//   · 控制面板、"此电脑"里的控制项这类虚拟对象（没有真实文件系统路径）
//     既不能复制也不能剪切。
// 压缩包文件本身和普通文件一样：zip/rar 都能剪（用户实测系统就是这样，之前
// 按"除 zip 外禁剪"是记错了，这里已改回）。
// 这些判断只查字符串，零成本（符合"右键菜单弹出不查注册表、不碰 COM"的要求）。
// ---------------------------------------------------------------------------
static bool CanCopyPath(const std::wstring& path)
{
    return shell::IsFileSystemPath(path);
}

static bool CanCutPath(const std::wstring& path)
{
    // 两条"不能剪"：驱动器、虚拟对象
    return shell::IsFileSystemPath(path) &&
           !shell::IsDriveRootPath(path);
}

// ---------------------------------------------------------------------------
// 压缩包内部的右键菜单（在 zip / rar / 7z 里选中文件时走这里）
//
// 系统实际给的项（用户实测资源管理器）：
//   · zip 内部      : 打开 / 剪切 / 复制 / 删除 / 属性
//   · 其它压缩包内部 : 打开 / 复制
//
// 但"剪切 / 删除 / 属性"要真能干活，必须走容器内项的 PIDL 通道：
// 容器里的项没有真实磁盘路径，路径字符串交给 Shell 解析不出来（实测会报
// "参数错误"，还有过把整个压缩包误删的先例）。这条通道还在做，
// 所以这里先只给「打开 / 复制」——宁可少给，也不给一个点了会出错/出事的项。
// 通道接好后再把 zip 的 剪切/删除/属性 补回来。
// ---------------------------------------------------------------------------
void MainWindow::BuildContainerItemMenu(HMENU menu)
{
    ::AppendMenuW(menu, MF_STRING, kCommandOpen, L"打开(&O)");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandCopy, L"复制(&C)");
}

void MainWindow::BuildSingleFileMenu(HMENU menu, const std::wstring& filePath)
{
    // 压缩包内部：按容器能力单独给菜单，早退
    if (m_currentIsContainer)
    {
        BuildContainerItemMenu(menu);
        return;
    }

    ::AppendMenuW(menu, MF_STRING, kCommandOpen, L"打开(&O)");

    // 压缩包文件本身和文件夹一样，也能"在新窗口中打开"（Ctrl+双击同理）
    if (m_menuPrimaryIsContainerFile)
    {
        ::AppendMenuW(menu, MF_STRING, kCommandOpenInNewWindow, L"在新窗口中打开(&N)");
    }

    // 打开方式：只给"普通文件"；菜单内容全部来自内存里的账本，零成本
    {
        const std::wstring extension = shell::ExtensionKeyForPath(filePath);

        if (!extension.empty() && shell::IsExtensionHandled(extension))
        {
            FillOpenWithMenu(menu, extension);
        }
    }

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (CanCutPath(filePath) ? 0u : MF_GRAYED), kCommandCut, L"剪切(&T)");
    ::AppendMenuW(menu, MF_STRING | (CanCopyPath(filePath) ? 0u : MF_GRAYED), kCommandCopy, L"复制(&C)");
    ::AppendMenuW(menu, MF_STRING, kCommandCopyPath, L"复制文件路径(&A)");

    // 撤销 / 恢复 紧跟在「复制」那一格的下一格（自带尾分隔线）
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    FillUndoItem(menu);

    // 按住 Shift 弹的菜单：「删除」改为永久删除（文字不变，行为变，和资源管理器一致）
    ::AppendMenuW(menu, MF_STRING,
                  m_menuPermanentDelete ? kCommandDeletePermanent : kCommandDelete,
                  L"删除(&D)");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandRename, L"重命名(&M)");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandProperties, L"属性(&S)");
}

// ---------------------------------------------------------------------------
// 单选一个文件夹
//
//   打开
//   在新窗口中打开
//   ─────
//   剪切
//   复制
//   复制文件路径        （只把路径文字放进剪贴板，Ctrl+Shift+C 也能触发）
//   ─────
//   撤销 / 恢复      （紧跟「复制」的下一格，有才出现）
//   ─────
//   删除
//   ─────
//   重命名
//   ─────
//   属性
// ---------------------------------------------------------------------------
void MainWindow::BuildSingleFolderMenu(HMENU menu, const std::wstring& folderPath)
{
    // 压缩包内部（不管 zip / rar / 7z）：一律只读，只给「打开 / 复制」，
    // 和系统的只读容器一致。（zip 内部的剪切 / 删除 / 属性还没接。）
    if (m_currentIsContainer)
    {
        BuildContainerItemMenu(menu);
        return;
    }

    ::AppendMenuW(menu, MF_STRING, kCommandOpen, L"打开(&O)");
    ::AppendMenuW(menu, MF_STRING, kCommandOpenInNewWindow, L"在新窗口中打开(&N)");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // 「主文件夹」里的系统入口（桌面 / 文档 / 下载 / 图片 / 音乐…）不给「剪切」：
    // 这些是 Shell 重定向的已知文件夹，位置由用户配置决定，搬不走；
    // 系统资源管理器的「主文件夹」里也没有这一项。
    if (!m_menuSuppressDeleteCut)
    {
        ::AppendMenuW(menu, MF_STRING | (CanCutPath(folderPath) ? 0u : MF_GRAYED), kCommandCut, L"剪切(&T)");
    }

    ::AppendMenuW(menu, MF_STRING | (CanCopyPath(folderPath) ? 0u : MF_GRAYED), kCommandCopy, L"复制(&C)");
    ::AppendMenuW(menu, MF_STRING, kCommandCopyPath, L"复制文件路径(&A)");

    // 撤销 / 恢复 紧跟在「复制」那一格的下一格（自带尾分隔线）
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    FillUndoItem(menu);

    // 按住 Shift 弹的菜单：「删除」改为永久删除（文字不变，行为变，和资源管理器一致）
    // 主文件夹里的系统入口同样不给「删除」（删掉就是删掉整个"桌面/文档"入口）
    if (!m_menuSuppressDeleteCut)
    {
        ::AppendMenuW(menu, MF_STRING,
                      m_menuPermanentDelete ? kCommandDeletePermanent : kCommandDelete,
                      L"删除(&D)");
    }

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandRename, L"重命名(&M)");

    // 「在终端中打开」：文件夹是真实目录，可以直接当终端的工作目录
    // （压缩包内部的子目录在上面已经早退了，走不到这里）
    if (shell::IsFileSystemPath(folderPath))
    {
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, kCommandOpenTerminal, L"在终端中打开");
    }

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandProperties, L"属性(&S)");
}

// ---------------------------------------------------------------------------
// 选中多个
//
//   打开
//   ─────
//   剪切
//   复制
//   复制文件路径        （多选时一行一条路径，Ctrl+Shift+C 也能触发）
//   ─────
//   撤销 / 恢复      （紧跟「复制」的下一格，有才出现）
//   ─────
//   删除
//   ─────
//   批量重命名          （多个一起改名，系统的「重命名」对多选只改一个，这里给它更全的）
//   ─────
//   属性
//
// 没有单个「重命名」（系统也不给多选逐个改名），没有新建，没有打开方式。
// ---------------------------------------------------------------------------
void MainWindow::BuildMultipleMenu(HMENU menu)
{
    // 压缩包内部多选：和单选文件一样按容器能力给项，早退
    if (m_currentIsContainer)
    {
        BuildContainerItemMenu(menu);
        return;
    }

    ::AppendMenuW(menu, MF_STRING, kCommandOpen, L"打开(&O)");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // 多选：只要里面有一项不能复制/剪切（驱动器、控制面板等），整条就灰掉
    const std::vector<std::wstring> selectedPaths = m_fileListView.GetSelectedPaths();
    bool canCut  = !selectedPaths.empty();
    bool canCopy = canCut;

    for (const auto& path : selectedPaths)
    {
        canCut  = canCut  && CanCutPath(path);
        canCopy = canCopy && CanCopyPath(path);
    }

    ::AppendMenuW(menu, MF_STRING | (canCut ? 0u : MF_GRAYED), kCommandCut, L"剪切(&T)");
    ::AppendMenuW(menu, MF_STRING | (canCopy ? 0u : MF_GRAYED), kCommandCopy, L"复制(&C)");
    ::AppendMenuW(menu, MF_STRING, kCommandCopyPath, L"复制文件路径(&A)");

    // 撤销 / 恢复 紧跟在「复制」那一格的下一格（自带尾分隔线）
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    FillUndoItem(menu);

    // 按住 Shift 弹的菜单：「删除」改为永久删除（文字不变，行为变，和资源管理器一致）
    ::AppendMenuW(menu, MF_STRING,
                  m_menuPermanentDelete ? kCommandDeletePermanent : kCommandDelete,
                  L"删除(&D)");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandBatchRename, L"批量重命名(&R)");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCommandProperties, L"属性(&S)");
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
// 「查看」子菜单（和资源管理器一样，当前视图打勾）
// ---------------------------------------------------------------------------
void MainWindow::FillViewMenu(HMENU parentMenu)
{
    HMENU viewMenu = ::CreatePopupMenu();
    if (viewMenu == nullptr)
    {
        return;
    }

    const FileListView::ViewMode current = m_fileListView.CurrentViewMode();

    ::AppendMenuW(viewMenu, MF_STRING | ((current == FileListView::ViewMode::LargeIcon) ? MF_CHECKED : 0),
                  kCommandViewLarge, L"大图标(&L)");
    ::AppendMenuW(viewMenu, MF_STRING | ((current == FileListView::ViewMode::SmallIcon) ? MF_CHECKED : 0),
                  kCommandViewSmall, L"小图标(&S)");
    ::AppendMenuW(viewMenu, MF_STRING | ((current == FileListView::ViewMode::List) ? MF_CHECKED : 0),
                  kCommandViewList, L"列表(&T)");
    ::AppendMenuW(viewMenu, MF_STRING | ((current == FileListView::ViewMode::Details) ? MF_CHECKED : 0),
                  kCommandViewDetails, L"详细信息(&D)");
    ::AppendMenuW(viewMenu, MF_STRING | ((current == FileListView::ViewMode::Tiles) ? MF_CHECKED : 0),
                  kCommandViewTiles, L"平铺(&I)");

    ::AppendMenuW(parentMenu, MF_STRING | MF_POPUP,
                  reinterpret_cast<UINT_PTR>(viewMenu), L"查看(&V)");
}

// ---------------------------------------------------------------------------
// 「新建文件夹」+「新建」子菜单（两者相邻，新建文件夹在上面）
// ---------------------------------------------------------------------------
void MainWindow::FillNewFolderAndNewMenu(HMENU menu)
{
    const std::wstring currentPath = m_history.Current();
    const bool canCreate = !currentPath.empty() &&
                           currentPath.compare(0, 2, L"::") != 0 &&
                           currentPath.compare(0, 6, L"shell:") != 0;

    ::AppendMenuW(menu, MF_STRING | (canCreate ? 0 : MF_GRAYED),
                  kCommandNewFolder, L"新建文件夹(&N)");

    FillNewMenu(menu);
}

// ----------------------------------------------------------------------------
// 打开方式子菜单 / 新建子菜单
//
// 这两块也是菜单代码（原来在 MainWnd.cpp），一起放在这个翻译单元里。
// 它们只读内存里的账本 / 新建项快照，弹出时零 IO。
// ----------------------------------------------------------------------------

void MainWindow::FillOpenWithMenu(HMENU parentMenu, const std::wstring& extension)
{
    if (parentMenu == nullptr)
    {
        return;
    }

    HMENU subMenu = ::CreatePopupMenu();
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

    ::AppendMenuW(parentMenu, MF_STRING | MF_POPUP,
                  reinterpret_cast<UINT_PTR>(subMenu), L"打开方式");
}

void MainWindow::FillNewMenu(HMENU parentMenu)
{
    if (parentMenu == nullptr)
    {
        return;
    }

    HMENU subMenu = ::CreatePopupMenu();
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

    ::AppendMenuW(parentMenu, MF_STRING | MF_POPUP,
                  reinterpret_cast<UINT_PTR>(subMenu), L"新建(&W)");
}

// ----------------------------------------------------------------------------
// 撤销/恢复菜单项 + 自绘菜单项绘制
//
// FillUndoItem 只读内存里的撤销栈；DrawMenuItem 是 WM_DRAWITEM 的实现，
// 用主窗口字体和预取好的小图标画一行。两块都只跟菜单有关。
// ----------------------------------------------------------------------------

void MainWindow::FillUndoItem(HMENU menu)
{
    bool anyItem = false;

    // 「撤销 删除」：撤销栈顶那一步
    if (const UndoRecord* top = m_undo.TopUndo())
    {
        const UndoRecord& record = *top;

        // 批量重命名单独说一句「撤销 批量重命名」，让用户一眼看出这一条
        // 和单文件改名不是同一档（也就不会奇怪 Ctrl+Z 为什么不动它）
        const wchar_t* verb = record.batchRename ? L"批量重命名"
                                                 : core::UndoStack::VerbText(record.kind);

        if (verb != nullptr)
        {
            const std::wstring text = std::wstring(L"撤销 ") + verb + L"(&U)";
            ::AppendMenuW(menu, MF_STRING, kCommandUndo, text.c_str());
            anyItem = true;
        }
    }

    // 「恢复 删除」：重做栈顶那一步（撤销过、还没被新操作冲掉的那些）
    if (const UndoRecord* top = m_undo.TopRedo())
    {
        const UndoRecord& record = *top;

        const wchar_t* verb = record.batchRename ? L"批量重命名"
                                                 : core::UndoStack::VerbText(record.kind);

        if (verb != nullptr)
        {
            const std::wstring text = std::wstring(L"恢复 ") + verb + L"(&E)";
            ::AppendMenuW(menu, MF_STRING, kCommandRedo, text.c_str());
            anyItem = true;
        }
    }

    if (anyItem)
    {
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
}

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
