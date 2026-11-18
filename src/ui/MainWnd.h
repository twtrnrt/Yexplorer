// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/MainWnd.h
//
// 主窗口（阶段 3 版本）。
//
// 界面结构（对齐资源管理器）：
//
//   ┌────────────────────────────────────────────────────┐
//   │ [←] [→] [↑]  [ 地址栏：当前路径              ]      │  导航栏
//   ├────────────────────────────────────────────────────┤
//   │                                                    │
//   │  文件列表（图标 + 名称 / 修改日期 / 类型 / 大小）    │
//   │                                                    │
//   ├────────────────────────────────────────────────────┤
//   │ 14 个项目                                          │  状态栏
//   └────────────────────────────────────────────────────┘
//
 // ============================================================================

#pragma once

#include <windows.h>
#include <commctrl.h>

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <functional>
#include <map>

#include "core/AsyncTask.h"
#include "core/FolderWatcher.h"
#include "core/NavigationHistory.h"
#include "shell/ShellFolder.h"
#include "shell/ShellIcon.h"
#include "shell/ShellOpenWith.h"
#include "ui/AddressBar.h"
#include "ui/FileListView.h"
#include "ui/FlatButton.h"
#include "ui/NavigationPane.h"
#include "ui/OpenWithDialog.h"
#include "ui/SettingsDialog.h"
#include "ui/MenuIcons.h"
#include "shell/ShellNew.h"

// 子控件 ID
static const int kControlIdBack    = 1001;
static const int kControlIdForward = 1002;
static const int kControlIdUp      = 1003;
static const int kControlIdAddress = 1004;
static const int kControlIdHome    = 1010;   // 主页（去「我的文档」）
static const int kControlIdRefresh = 1011;   // 刷新
static const int kControlIdThisPc  = 1012;   // 计算机图标（去「此电脑」）
static const int kControlIdSettings = 1013;  // 设置（地址栏右端的齿轮）

// 快捷键命令 ID（和控件 ID 分开编号，避免撞号）
static const int kCommandCopy      = 3101;   // Ctrl+C
static const int kCommandCut       = 3102;   // Ctrl+X
static const int kCommandPaste     = 3103;   // Ctrl+V
static const int kCommandDelete    = 3104;   // Delete
static const int kCommandRefresh   = 3105;   // F5
static const int kCommandNewFolder = 3106;   // Ctrl+Shift+N
static const int kCommandSelectAll = 3107;   // Ctrl+A
static const int kCommandGoUp      = 3108;   // Backspace

// 右键菜单里的命令
static const int kCommandOpen       = 3201;  // 打开
static const int kCommandProperties = 3202;  // 属性
static const int kCommandOpenInNewWindow = 3203;  // 在新窗口中打开
static const int kCommandUndo       = 3204;  // 撤销（Ctrl+Z）
static const int kCommandRedo       = 3205;  // 恢复/重做（Ctrl+Y）
static const int kCommandCopyPath   = 3206;  // 复制文件路径（Ctrl+Shift+C）
// 下面这两个只给快捷键用：Ctrl+Z / Ctrl+Y 发的是它们，不等于右键菜单里的
// 「撤销」「恢复」。拆开的原因是批量重命名只能从菜单撤销，不能走键盘（用户拍板）。
static const int kCommandUndoKeyboard = 3207;  // Ctrl+Z 专用（批量重命名不响应）
static const int kCommandRedoKeyboard = 3208;  // Ctrl+Y 专用（批量重命名不响应）

// 「打开方式」子菜单里的命令
//   3400 ~ 3499 分给"账本里第 N 个程序"（3400 + 下标），所以下标最多到 99
static const int kCommandOpenWithManage = 3210;   // 添加或管理程序…
static const int kCommandOpenWithBase   = 3400;   // 用列表里第 N 个程序打开
static const int kCommandOpenWithSelf   = 3212;   // 用本程序打开（压缩包专用入口）

// 右键菜单「新建」子菜单里的命令
//   3500 ~ 3599 分给"新建项快照里第 N 项"
static const int kCommandNewBase        = 3500;

// 右键菜单「查看」子菜单（和资源管理器一样，几种视图里挑一个）
static const int kCommandViewLarge   = 3301;  // 大图标
static const int kCommandViewSmall   = 3302;  // 小图标
static const int kCommandViewList    = 3303;  // 列表
static const int kCommandViewDetails = 3304;  // 详细信息
static const int kCommandViewTiles   = 3305;  // 平铺（此电脑那种带容量进度条的）

// 重命名快捷键
static const int kCommandRename     = 3109;  // F2

// 批量重命名（选中多个时右键里那一条）
static const int kCommandBatchRename = 3211;

// ---- 2026-11-18 新增：对齐 Windows 11 资源管理器的快捷键 ----
//
// 这些都是"键盘专用"命令：菜单里没有对应项，或者菜单项另有一个 id
// （比如菜单里的「删除」走 kCommandDelete，键盘 Shift+Delete 走永久删除）。
static const int kCommandNewWindow       = 3213;  // Ctrl+N：新窗口（选中文件夹开它，否则当前目录）
static const int kCommandHomeWindow      = 3214;  // Ctrl+T：开一个「主文件夹」窗口
static const int kCommandCloseWindow     = 3215;  // Ctrl+W：关闭当前窗口
static const int kCommandGoBackKey       = 3216;  // Alt+←：后退（工具栏后退按钮是控件 ID，不同）
static const int kCommandGoForwardKey    = 3217;  // Alt+→：前进
static const int kCommandFocusAddressBar = 3218;  // Alt+D / Ctrl+L：焦点跳到地址栏
static const int kCommandDeletePermanent = 3219;  // Shift+Delete / Shift+右键删除：永久删除（带确认）
static const int kCommandAutoFitColumns  = 3220;  // Ctrl+加号：列宽自适应内容

// 右键「在终端中打开」（文件夹、文件夹空白处、盘符根都给）
static const int kCommandOpenTerminal   = 3221;

class MainWindow
{
public:
    MainWindow();
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // initialPath      —— 启动时直接打开的目录（空 = 按上次停的目录启动）
    // initialSelectFile —— 当命令行传进来的是"文件"时，这里是那个文件本身：
    //                      目录会打开它所在的文件夹，首次枚举完成后把这一项选中
    bool Create(HINSTANCE hInstance,
                const std::wstring& initialPath = std::wstring(),
                const std::wstring& initialSelectFile = std::wstring());
    void Show(int nCmdShow);
    int  RunMessageLoop();

    HWND Handle() const { return m_hWnd; }

private:
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    // 创建工具栏按钮、地址栏、状态栏、字体
    void CreateChildControls();

    // 用户主动导航（会记入历史）
    void NavigateToPath(const std::wstring& path);

    // ----------------------------------------------------------------
    // 导航来源（决定"地址栏上写什么名字"和"向上按钮去哪儿"）
    //
    // 从「主文件夹」双击「文档」进去以后：
    //   地址栏显示「文档」，而不是 C:\Users\user\Documents；
    //   向上按钮回「主文件夹」，而不是它的真实父目录 C:\Users\user。
    // 这就是系统资源管理器的表现——它记住的是"从哪儿点进来的"。
    // 所以我们给每个"从列表里双击进去的目录"记一条来源。
    // ----------------------------------------------------------------
    struct NavigationOrigin
    {
        std::wstring displayName;   // 地址栏/标题上显示的名字（如「文档」）
        std::wstring parentPath;    // 向上按钮要去的地方（如「主文件夹」）
    };

    // parentPath 留空表示"上一级就是进来之前待着的那个目录"（列表双击走这条）；
    // 传了具体路径就用它（左栏"常用位置"那几项固定回「主文件夹」）。
    void RecordNavigationOrigin(const std::wstring& path,
                                const std::wstring& displayName,
                                const std::wstring& parentPath = L"");

    // 当前目录给人看的名字：有来源记录就用记录的名字，否则按路径推导
    std::wstring DisplayNameFor(const std::wstring& path);

    // 后退 / 前进 / 向上
    void GoBack();
    void GoForward();
    void GoUp();

    // 启动后台枚举（只负责枚举与界面刷新，不改历史）
    void StartEnumeration(const std::wstring& path);

    // 窗口尺寸 / 视图变化后，按控件真实列数重排「此电脑」的分组标题行
    // （只在列数变化时才重排，平时什么都不做）
    void RefreshThisPcGrouping();

    // 重新枚举当前目录（复制/删除/新建之后调用）
    void RefreshCurrentFolder();

    // ---- 文件操作（阶段 4）----
    void DoCopy();          // Ctrl+C：记住选中的路径
    void DoCopyPath();      // Ctrl+Shift+C / 右键「复制文件路径」：把路径文字放进剪贴板
    void DoCut();           // Ctrl+X：记住选中的路径，粘贴时移动
    void DoPaste();         // Ctrl+V：复制/移动到家目录
    // Delete：删到回收站；permanent = true 时永久删除（Shell 会弹"确实要永久删除"确认框）
    void DoDelete(bool permanent = false);

    // 按路径从当前枚举结果里取绝对 PIDL（压缩包内部删除等操作用）
    bool SelectedEntryPidls(const std::vector<std::wstring>& paths,
                            std::vector<std::vector<unsigned char>>& pidls);
    void DoNewFolder();     // Ctrl+Shift+N：新建文件夹

    // 批量重命名（选中多个时右键「批量重命名」）
    void DoBatchRename();

    // 在指定文件夹里开一个终端窗口（右键「在终端中打开」）
    // 优先 Windows Terminal，没有就退回 cmd.exe
    void DoOpenTerminalIn(const std::wstring& folderPath);

    // ---- 后台执行文件操作（避免复制大文件时界面卡住）----
    void RunFileOperationAsync(std::function<HRESULT()> operation);

    // ---- 拖放：接收别的程序拖进来的文件 ----
    void RegisterDropTargets();      // 列表 + 左栏都注册
    void UnregisterDropTargets();

    void PerformDropFromOutside(const std::vector<std::wstring>& sources,
                                const std::wstring&              targetDirectory,
                                DWORD                            effect,
                                bool                             createShortcut);

    // ---- 右键菜单 ----
    //
    // 资源管理器的右键菜单不是一套，而是按"点到什么"重新组合的。
    // 这里同样分场景构建，不同场景只出现该有的项：
    //
    //   Background     文件夹空白处：查看 / 粘贴 / 新建文件夹 / 新建 / 刷新 / 属性
    //   SingleFile     单选一个文件：打开 / 打开方式 / 剪切 / 复制 / 删除 / 重命名 / 属性
    //   SingleFolder   单选一个文件夹：打开 / 在新窗口中打开 / 剪切 / 复制 / 删除 / 重命名 / 属性
    //   Multiple       选中多个：打开 / 剪切 / 复制 / 删除 / 属性
    //   ShellItem      盘符、此电脑、控制面板这类：打开 / 在新窗口中打开 / 属性
    //   None           组标题、分隔行：不弹菜单
    enum class ContextScene
    {
        None,
        Background,
        SingleFile,
        SingleFolder,
        Multiple,
        ShellItem,
    };

    void ShowContextMenu(POINT screenPoint);

    // 按场景拼菜单（都是零成本：只用已经加载在内存里的条目数据）
    void BuildBackgroundMenu(HMENU menu);
    void BuildSingleFileMenu(HMENU menu, const std::wstring& filePath);
    void BuildSingleFolderMenu(HMENU menu, const std::wstring& folderPath);
    void BuildMultipleMenu(HMENU menu);
    void BuildShellItemMenu(HMENU menu, const std::wstring& itemPath);

    // 压缩包内部的右键菜单（zip 给 打开/剪切/复制/删除/属性；其它给 打开/复制）
    void BuildContainerItemMenu(HMENU menu);

    // 当前右键选中的那一项是不是"压缩包文件"（zip/rar/7z 这类容器）
    // 由 ShowContextMenu 在拼菜单前填好；BuildSingleFileMenu 靠它决定
    // 要不要多给一条「在新窗口中打开」。
    bool m_menuPrimaryIsContainerFile = false;

    // 这次右键是"按住 Shift 点的"吗？
    // 是的话，「删除」执行永久删除（不进回收站），菜单文字保持和系统一样不变。
    // 由 ShowContextMenu 在拼菜单前填好。
    bool m_menuPermanentDelete = false;

    // 查看 / 新建文件夹 + 新建 这两块公共片段
    void FillViewMenu(HMENU parentMenu);
    void FillNewFolderAndNewMenu(HMENU menu);

    // ---- 撤销 / 恢复（多级，和资源管理器一样 Ctrl+Z / Ctrl+Y）----
    //
    // 每一步记成一条 UndoRecord：操作前在哪（beforePaths）、操作后在哪（afterPaths）。
    // 撤销 = 按相反方向做一遍，做完把这条压进重做栈；重做 = 再按原方向做一遍。
    //
    // 菜单文案跟系统一样，显示「撤销 删除」「撤销 重命名」「撤销 移动」
    // 「撤销 复制」「撤销 新建」；栈空时这一项整条不出现。
    enum class UndoKind
    {
        None,
        Create,     // 新建（文件夹 / 新建菜单里建出来的文件）
        Rename,     // 重命名
        Move,       // 剪切粘贴（移动）
        Copy,       // 复制粘贴
        Delete,     // 删除（进回收站，撤销 = 从回收站还原）
    };

    struct UndoRecord
    {
        UndoKind                  kind = UndoKind::None;
        std::vector<std::wstring> afterPaths;    // 操作之后这些项在哪（撤销要动它们）
        std::vector<std::wstring> beforePaths;   // 操作之前在哪（重命名前的全路径 / 移动和删除的源路径）

        // 批量重命名专用的标记：这条记录照样在撤销栈里、右键「撤销」能撤，
        // 但 Ctrl+Z / Ctrl+Y 会被拦掉（用户拍板：批量改名不能走快捷键）。
        bool batchRename = false;
    };

    // 最多记多少级（再多也没人真的会连撤 50 步，省点内存）
    static const size_t kMaxUndoSteps = 50;

    std::vector<UndoRecord> m_undoStack;   // 撤销栈（末尾是最新的一步）
    std::vector<UndoRecord> m_redoStack;   // 重做栈（撤销过的步骤按顺序排在这里）

    void PushUndoRecord(const UndoRecord& record);   // 操作成功后压栈（同时清空重做栈）
    void FillUndoItem(HMENU menu);                   // 有可撤销/可恢复的步骤才往菜单里加
    void PerformUndo(bool fromKeyboard);             // fromKeyboard=true 是 Ctrl+Z，false 是右键「撤销」
    void PerformRedo(bool fromKeyboard);             // fromKeyboard=true 是 Ctrl+Y，false 是右键「恢复」
    void ClearUndoHistory();                         // 清空两栈

    // 批量改名记录的撤销/恢复：两段式把一批文件改成另一批名字，返回失败项数
    int RenameManyTwoStage(const std::vector<std::wstring>& fromPaths,
                           const std::vector<std::wstring>& toNames);

    // 把操作的动词转成文案（「撤销 删除」里的"删除"）
    static const wchar_t* UndoVerbText(UndoKind kind);

    // 在另一个窗口里打开这个目录（「在新窗口中打开」/ 按住 Ctrl 双击）。
    // 已经有窗口停在这个目录时会把它拉到前台，不再开新窗口。
    //
    // forceNewWindow = true：跳过上面那条"已有窗口就拉前台"的去重，一定开新窗口。
    // Ctrl+N / Ctrl+T 用这个（用户要求：当前目录也照开一份新的）。
    void OpenFolderInNewWindow(const std::wstring& folderPath, bool forceNewWindow = false);

    // Ctrl+双击：文件夹 → 在新窗口打开（返回 true 表示已处理）；
    // 文件/标题行等返回 false，交给默认的导航/打开逻辑
    bool ActivateItemInNewWindow(size_t index);

    // 别的窗口转来"打开这个目录"的请求、且当前目录正是它：拉到前台（可选选中某个文件）
    void ActivateFromRelay(const std::wstring& selectFile);

    void ShowPropertiesForSelection();

    // 按指定路径弹系统的「属性」窗口（左栏右键、列表里选中一项时都用它）
    void ShowPropertiesForPath(const std::wstring& path);

    // ---- 左栏（导航栏）右键菜单 ----
    //
    // 导航栏只上报"右键了哪个节点"，菜单构建和命令执行都在这里做，
    // 保证和文件列表里"文件夹右键"是同一套动作。
    void ShowNavigationContextMenu(const std::wstring& path, bool isPinned,
                                   bool isFolder, POINT screenPoint);

    // 把某个路径从常用区（固定区）里去掉：删配置 → 存盘 → 重建左栏
    void UnpinPinnedPath(const std::wstring& path);

    // 「新建」菜单（二级菜单：文件夹 + 系统 ShellNew 里登记的各类新文件模板）
    void FillNewMenu(HMENU parentMenu);

    // 按「新建」快照里第 index 项建一个东西出来（文件夹 / 文本文档 / 快捷方式…）
    void DoNewItem(size_t index);

    // 启动后把「新建」项枚举出来、把菜菜单图标取好（都在后台线程做）
    void StartBackgroundMenuData();

    // ---- 打开选中项 ----
    void OpenSelection();

    // ----------------------------------------------------------------
    // 「打开方式」（自研账本，不受系统关联影响）
    //
    // 规则（和用户对齐过的）：
    //   · 双击 / 右键「打开」永远用账本里记的默认程序；
    //   · 某个扩展名第一次用时弹一次设置窗口，设好就固定；
    //   · 设置窗口里点「跳过」= 这次按系统默认打开、不记录、下次还会问；
    //   · 默认程序被卸载 → 提示并让用户重选；
    //   · 右键「打开方式」子菜单只读内存里的账本，绝不读注册表
    //     （用户要求：影响右键菜单速度的事情非必要不做）。
    // -------------------------------------------------------------

    // 右键菜单里的「打开方式」子菜单（零成本：只读已经加载的配置）
    void FillOpenWithMenu(HMENU parentMenu, const std::wstring& extension);

    // 用账本里第 programIndex 个程序打开所有选中文件
    void OpenSelectionWithProgram(size_t programIndex);

    // 打开某扩展名的设置窗口（manageOnly=false 时是"第一次询问"）
    // 打开「打开方式」设置窗口。
    //   filePath 非空 = 从"打开某个文件"触发（窗口里有「打开」按钮）；
    //   为空 = 从右键「添加或管理程序」触发。
    // 用户在窗口里点了「打开」就顺手用默认程序把这个文件打开。
    void ShowOpenWithSettings(const std::wstring& extension, bool firstTime,
                              const std::wstring& filePath = L"");

    // 按账本打开一个文件。返回 true 表示"这件事我们管了"（成功或已给用户提示），
    // 返回 false 表示"不归我们管"（特殊类型、或者没配置又跳过了），调用方走原来的逻辑。
    bool TryOpenFileWithOurChoice(const std::wstring& filePath, const std::wstring& extension);

    // 双击压缩包（zip / rar / 7z…）的打开判定：走「打开方式」账本，
    // 没设过就弹设置窗口并默认用本程序，设过就按设过的打开。
    void OpenContainerEntry(const shell::ShellEntry& entry);

    // 进压缩包容器里浏览（OpenContainerEntry 里「用本程序打开」那条路）
    void NavigateContainerEntry(const shell::ShellEntry& entry);

    // ----------------------------------------------------------------
    // 打开容器内部的一个文件：先解成只读临时文件，再打开那个临时文件
    //
    // 和资源管理器一个做法：容器里的路径（C:\a.rar\inner.txt）磁盘上不存在，
    // 直接把路径交给 Shell 打不开。所以先解到
    //   %TEMP%\<本进程固定的 guid>_<压缩包文件名>\
    // 再启动解出来的真实文件。
    //
    // 返回解出来的真实路径；失败返回空串并填 error（中文，可直接弹给用户）。
    // ----------------------------------------------------------------
    std::wstring ExtractContainerEntryToTemp(const std::wstring& virtualPath,
                                             std::wstring& error);

    // 这个压缩包里的东西解到哪个临时目录。
    // 返回空串表示取不到临时目录（调用方自己决定怎么报错）。
    std::wstring ContainerTempDirectoryFor(const std::wstring& containerRoot);

    // ----------------------------------------------------------------
    // 拖出前的路径准备（文件列表的 LVN_BEGINDRAG 会调它）
    //
    // 普通磁盘文件 → 原样返回，零成本（不查磁盘、不碰 COM）。
    // 压缩包内部的项 → 先解到临时目录，返回真实路径；解不出来的项跳过。
    // 一项都没解出来时返回空，调用方据此放弃这次拖动。
    //
    // 解压放在后台线程做，这里每 100 毫秒看一眼"落点文件长到多大了"，
    // 进度窗显示的是真实解压进度（不是按项数估的）。
    // ----------------------------------------------------------------
    std::vector<std::wstring> PrepareDragPaths(const std::vector<std::wstring>& paths);

    // 「正在解出以便拖出」的小进度窗（拖动前解压时挡住视线的那块）。
    // 只有总大小超过阈值才显示，解完立即销毁。percent 传 0~100。
    void ShowDragProgress(const std::wstring& text, int percent);
    void HideDragProgress();

    // 把某条程序的存在状态记进配置（只在值真的变了才写盘，避免频繁落盘）
    void SetOpenWithProgramAlive(const std::wstring& extension,
                                 const std::wstring& programKey,
                                 int alive);

// 左栏里点了一项：是目录就进去，是程序/快捷方式就启动它
void OpenPinnedOrFolder(const std::wstring& path);

// 把拖进左栏固定区的东西记进配置文件（只记路径，不动文件本身）
void AddPinnedItems(const std::vector<std::wstring>& sources);

    // ---- 排序（点表头）----
    void OnColumnClick(int column);
    void SortEntries();          // 按当前排序状态排一遍数据

    // ---- 重命名 ----
    void DoRename(size_t index, const std::wstring& newName);

    // 取当前结果（加锁拿一份引用，避免和枚举线程抢）
    std::shared_ptr<shell::EnumerateResult> CurrentResult();

    // 更新状态栏和标题里的项目数（局部增删后要刷新这两个地方）
    void UpdateTitle();

    // 后台枚举完成
    void OnEnumerationDone();

    // 双击条目
    void OnItemActivated(size_t index);

    // 重新布局所有子控件
    void LayoutChildren();

    // 当前左栏宽度（还没拖过时用默认值 240）
    int CurrentPaneWidth() const { return m_navWidth > 0 ? m_navWidth : Scale(240); }

    // 鼠标是不是压在「左栏和列表中间那条竖线」上（热区比线本身宽几像素，方便抓住）
    bool IsOnPaneDivider(int clientX) const;

    // 拖动分隔线：把左栏宽度改成鼠标所在的位置
    void UpdatePaneWidth(int clientX);

    // 按 DPI 缩放
    int Scale(int value) const;

    // 刷新导航按钮可用状态、状态栏文本
    void UpdateNavigationState();
    void UpdateStatusBar();

    // 当前目录里「真正有多少项」——分组标题行不算（标题、状态栏都按它显示）
    size_t RealItemCount();

    // 切换视图模式（modeIsUserChoice 为真表示是用户从菜单选的，之后不再自动改）
    void ApplyViewMode(FileListView::ViewMode mode, bool modeIsUserChoice);

    // ---- 阶段 10：把窗口状态记住 / 恢复回来 ----
    // 恢复（启动时调一次）：左栏宽度、三列宽度、上次的查看模式
    void RestoreSavedLayout();

    // 记住（关窗口时、拖完分隔线时、拖完表头列宽时调）：
    // 窗口位置大小与最大化、左栏宽度、三列宽度、当前查看模式、当前目录
    void SaveWindowState();

    // 按"当前目录是什么类型"给出该用的默认视图
    FileListView::ViewMode DefaultViewModeForCurrentPath() const;

    // 独立记忆视图的位置（此电脑 / 控制面板）：
    // 是这类位置就返回它的内部标识，普通目录返回空串。
    // 判定只看路径字符串，不查注册表、不调 COM、不探测文件。
    std::wstring ViewLocationKeyForPath(const std::wstring& path) const;

    // 某个位置该用的视图：配置里记过就用记的，没记过用它自己的默认（现在都是平铺）
    FileListView::ViewMode DefaultViewModeForLocation(const std::wstring& key) const;

private:
    HWND      m_hWnd      = nullptr;
    HINSTANCE m_hInstance = nullptr;

    // 普通目录那套视图（"全局"）。进「此电脑」「控制面板」时会临时切到它们自己的模式，
    // 出来后要按这个值恢复，否则会被那两处的记忆带跑。-1 表示还没定过。
    int m_globalViewMode = -1;

    // 启动时命令行指定的目录（「在新窗口中打开」用它开新窗口）。
    // 空表示没指定，按上次停的目录启动。
    std::wstring m_startupPath;

    // 启动时命令行传进来的是文件 → 这里是那个文件的完整路径。
    // 首次枚举完成后选中它并滚到可见，选完立刻清空（以后刷新不再抢选中）。
    std::wstring m_startupSelectFile;

    // ---- 视图模式 ----
    // 当前列表视图（大图标/小图标/列表/详细信息）。
    // 进入新目录时按"这个目录该用哪种视图"重新定一次（系统文件夹默认小图标，
    // 普通磁盘目录默认详细信息），用户从「查看」菜单手动选过之后就一直用他选的。
    FileListView::ViewMode m_viewMode        = FileListView::ViewMode::Details;
    bool                   m_viewModeManual  = false;   // 用户是否手动指定过视图

    // ---- 子控件 ----
    FileListView m_fileListView;
    NavigationPane m_navigationPane;   // 左侧导航栏（快速访问 / 各硬盘）
    FlatButton   m_backButton;         // 后退（自绘扁平按钮）
    FlatButton   m_forwardButton;      // 前进
    FlatButton   m_upButton;           // 向上
    FlatButton   m_homeButton;         // 主页（点一下去「我的文档」）
    FlatButton   m_refreshButton;      // 刷新
    FlatButton   m_thisPcButton;       // 计算机图标（地址栏左端，点了去「此电脑」）
    FlatButton   m_settingsButton;     // 设置（齿轮，地址栏右端）
    AddressBar   m_addressBar;         // 地址栏（分段可点击，比如 C: > Windows）

    HWND         m_hStatusBar     = nullptr;
    HFONT        m_hUiFont        = nullptr;   // 普通控件字体
    HACCEL       m_hAccelerator   = nullptr;   // 键盘快捷键表

    // ---- 内部剪贴板（复制/剪切用）----
    // 说明：这里不写系统剪贴板，只在程序内部记住路径。
    // 好处是实现简单可靠；代价是不能和资源管理器互相复制粘贴（二期可扩展）
    std::vector<std::wstring> m_clipboardPaths;
    bool                      m_clipboardIsCut = false;

    // 粘贴是"剪切"时，操作成功后要清空剪贴板；成功前不动，失败还能重试
    bool                      m_clearClipboardWhenDone = false;

    // ---- 数据 ----
    shell::IconCache   m_iconCache;
    NavigationHistory  m_history;

    // ---- 右键菜单用的小图标（后台预取，菜单只查内存）----
    MenuIconCache      m_menuIcons;

    // ---- 「新建」项快照（后台枚举一次，菜单只读这份）----
    std::vector<shell::ShellNewItem> m_shellNewItems;
    std::mutex                       m_shellNewMutex;
    std::thread                      m_shellNewThread;

    // 自绘菜单项的数据。
    // 菜单还开着的时候必须一直活着（WM_MEASUREITEM / WM_DRAWITEM 会取它），
    // 所以每次弹菜单前清空、弹完再清空，期间由这个 vector 兜住。
    // 用 unique_ptr 是为了地址稳定：vector 扩容搬家时数据对象本身不会动。
    struct MenuDrawItem
    {
        std::wstring text;              // 菜单项文字
        HICON        icon = nullptr;    // 16px 图标（可为空：画空位，不探测）
    };

    std::vector<std::unique_ptr<MenuDrawItem>> m_menuDrawItems;

    // 画一个自绘菜单项（WM_DRAWITEM 里调）
    void DrawMenuItem(const DRAWITEMSTRUCT* drawItem);

    // 导航来源表：key 是小写路径，value 是"从哪儿进来的、叫什么名字"。
    // 见上面 NavigationOrigin 的说明。只在列表里双击进入时写入，
    // 后退/前进/地址栏跳转都沿用它，所以来回切也还是显示「文档」。
    std::map<std::wstring, NavigationOrigin> m_navOrigins;

    // ---- 后台枚举状态 ----
    std::thread                                  m_enumThread;
    core::CancellationToken                      m_enumToken;
    std::mutex                                   m_resultMutex;
    std::shared_ptr<shell::EnumerateResult>      m_result;

    // ---- 当前目录的变化监视（别的程序改了文件时自动刷新，不用按 F5）----
    core::FolderWatcher                          m_watcher;

    // ---- 后台文件操作状态（复制大文件时界面不卡住）----
    std::thread                                  m_operationThread;
    std::shared_ptr<std::atomic<bool>>           m_operationRunning;

    // 这次后台操作是干什么的（决定完成后是整表刷新还是局部增删）
    enum class FileOperationKind { None, Copy, Move, Delete };
    FileOperationKind                            m_lastOperation = FileOperationKind::None;

    // 这次删除是不是"永久删除"（Shift+Delete / Shift+右键）。
    // 永久删除不记撤销记录（文件已经进不了回收站，记了也还原不了）。
    bool                                         m_lastOperationPermanent = false;

    // 复制/移动完成后的那次刷新，是否要把"新出现的项"选中（和资源管理器一致）
    bool                                         m_selectNewAfterEnum = false;

    // ----------------------------------------------------------------
    // 当前显示的是不是「压缩包内部」
    //
    // 枚举结果里带回来的两个标记（见 shell::EnumerateResult）：
    //   m_currentIsContainer    —— 当前目录是压缩包这类容器
    //   m_currentIsZipContainer —— 且是 .zip（系统支持写入：能剪、能删）
    //
    // 用途：右键菜单里该出现哪些项、Ctrl+X / 删除要不要生效。
    // 存成成员是为了右键菜单弹出时零成本（不查磁盘、不碰 COM）。
    // ----------------------------------------------------------------
    bool                                         m_currentIsContainer = false;
    bool                                         m_currentIsZipContainer = false;

    // 解容器内部文件用的临时目录前缀（"{guid}_" 这一段）
    //
    // 只在本进程第一次需要时生成一次：同一个压缩包反复双击里面的文件，
    // 会一直用同一个临时目录（%TEMP%\{guid}_{压缩包名}），不会每次都堆一个新目录。
    std::wstring                                 m_containerTempTag;

    // 「正在解出以便拖出」小浮窗的三个控件（第一次需要时才创建，用完销毁）
    HWND                                         m_dragProgressPanel = nullptr;
    HWND                                         m_dragProgressText  = nullptr;
    HWND                                         m_dragProgressBar   = nullptr;

    std::vector<std::wstring>                    m_operationPaths;   // 操作涉及的路径

    // 上次给「此电脑」排分组标题时用的"一行几格"。
    // 窗口宽度一变这个数就变，RefreshThisPcGrouping 拿它做比较。
    int                                          m_thisPcPerRow = 0;

    // 这次粘贴的目标目录（撤销「复制/移动」时要对着它算"操作后在哪"）
    std::wstring                                 m_operationTargetDirectory;

    // ---- 排序状态 ----
    int                                          m_sortColumn    = 0;      // 0 名称 1 日期 2 类型 3 大小
    bool                                         m_sortAscending = true;
    bool                                         m_resultReady = false;
    double                                       m_elapsedMs   = 0.0;
    std::wstring                                 m_enumPath;

    // ---- 当前窗口标题 ----
    // 记一份，只在标题真的变了才去设置：
    // 每次刷新都重设标题会让任务栏按钮重新生成，视觉上窗口会"闪一下"
    std::wstring                                 m_lastTitle;

    // ---- 状态栏最后写进去的文字 ----
    // 同样只在文字真的变了才重设，否则「14 个项目」和「正在读取…」来回切，
    // 状态栏看着一直在闪
    std::wstring                                 m_lastStatusText;

    // ---- 左栏宽度（拖中间那条分隔线时改变）----
    // 拖动只改这一次运行里的值；松手那一刻会写进配置文件（阶段 10 已做）
    int  m_navWidth         = 0;       // 0 表示还没拖过，用默认宽度
    bool m_draggingSplitter = false;   // 是不是正在拖那条线
    bool m_paneVisible      = false;   // 左栏当前是否显示（窗口太窄时会收起来）

    // 启动时要不要直接最大化（按上次关窗口时的状态）
    bool m_startMaximized   = false;

    // 两条分隔线的位置：LayoutChildren 里算好，WM_PAINT 直接照着画
    RECT m_addressDividerRect = {};    // 地址栏下面那条横线
    RECT m_paneDividerRect    = {};    // 左栏右边那条竖线

    // 上一次布局时两条线的位置。
    // 用途：窗口变窄/变矮以后，线会跟着变短，旧线多出来的那一截要把重画区域
    // 一起算进来，否则旧线会留在界面上擦不掉。
    RECT m_prevAddressDividerRect = {};
    RECT m_prevPaneDividerRect    = {};
};
