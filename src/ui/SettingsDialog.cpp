// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/SettingsDialog.cpp
//
// 「设置」窗口的实现。说明见 SettingsDialog.h。
//
// 【为什么白底】
//   里面唯一的系统控件是复选框/单选，系统控件会自己刷一块白底；
//   窗口底色也用白色，两者就融在一起了，不会出现"白方块"。
// ============================================================================

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shobjidl.h>

#include "ui/SettingsDialog.h"

#include "core/Logger.h"
#include "core/Settings.h"
#include "shell/AutoStart.h"
#include "shell/DefaultManager.h"
#include "shell/ExplorerProxy.h"
#include "shell/ListaryIntegration.h"
#include "ui/BackgroundHost.h"
#include "ui/FlatDraw.h"

namespace
{

const wchar_t* kClassName = L"FileExplorerSettingsDialog";

const int kClientWidth  = 420;
const int kClientHeight = 780;
const int kPadding      = 24;

// 控件 ID
const int kControlIdMenuIcons      = 4001;
const int kControlIdClose          = 4002;
const int kControlIdShowHidden     = 4003;
const int kControlIdShowSystem     = 4004;
const int kControlIdShellWindow    = 4005;
const int kControlIdDefaultManager = 4006;
const int kControlIdProxy          = 4007;
const int kControlIdListary        = 4008;
const int kControlIdRadioThisPc    = 4010;
const int kControlIdRadioHome      = 4011;
const int kControlIdRadioCustom    = 4012;
const int kControlIdRadioLast      = 4013;
const int kControlIdBackground     = 4014;
const int kControlIdWinE           = 4015;
const int kControlIdTabs           = 4016;
const int kControlIdSingleInstance = 4017;

const COLORREF kBackgroundColor = RGB(255, 255, 255);
const COLORREF kTitleColor      = RGB(32, 32, 32);
const COLORREF kHintColor       = RGB(96, 103, 111);
const COLORREF kErrorColor      = RGB(178, 34, 34);
const COLORREF kDividerColor    = RGB(232, 235, 239);

const wchar_t* kTitleText = L"设置";

const wchar_t* kHintText1 = L"勾选后，「打开方式」和「新建」菜单里的每一项前面会显示小图标。";
const wchar_t* kHintText2 = L"不勾选就是纯文字菜单（默认，弹出更快）。改动立即保存。";

const wchar_t* kHiddenCheckText = L"显示隐藏的文件和文件夹";
const wchar_t* kSystemCheckText = L"显示受保护的操作系统文件";
const wchar_t* kHiddenHintText  = L"隐藏项用浅灰文字标示。改动立即生效，不用重启程序。";

const wchar_t* kStartupTitle = L"启动位置（无参数启动 / Win+E 都用它）";
const wchar_t* kRadioThisPcText = L"此电脑";
const wchar_t* kRadioHomeText   = L"主文件夹";
const wchar_t* kRadioCustomText = L"自定义文件夹";
const wchar_t* kRadioLastText   = L"最后一次打开的文件夹";
const wchar_t* kStartupHintText = L"自定义目录不存在时回退到主文件夹；显式带路径启动不受影响。";

const wchar_t* kQuickTitle = L"快速启动";
const wchar_t* kBackgroundCheckText = L"开机时后台启动（托盘常驻，关闭窗口不退出）";
const wchar_t* kBackgroundHintText  = L"登录时启动一个隐藏窗口，之后打开会快很多。";
const wchar_t* kWinECheckText = L"接管 Win+E（勾选时自动开启上面的后台启动）";
const wchar_t* kWinEHintText  = L"按 Win+E 新开一个窗口，停在上面选择的位置。";

const wchar_t* kWindowTitle = L"窗口模式";
const wchar_t* kTabsCheckText = L"多标签模式（关掉就是一个窗口一个目录）";
const wchar_t* kSingleInstanceCheckText = L"只允许一个实例（所有打开都进现有窗口）";

const wchar_t* kSectionTitle = L"注册与集成（可分别勾选，互不影响）";

const wchar_t* kDefaultCheckText = L"注册为默认文件管理器（文件夹和磁盘）";
const wchar_t* kDefaultHintText  = L"只改当前用户注册表；双击文件夹/磁盘用本程序打开。";

const wchar_t* kProxyCheckText = L"部署 explorer.exe 代理（需要管理员）";
const wchar_t* kProxyHintText  = L"替换 System32 里的 explorer.exe；微信等硬编码调用才会走本程序。";

const wchar_t* kListaryCheckText = L"安装 Listary 快速切换插件（需要管理员）";
const wchar_t* kListaryHintText  = L"写入 Listary 插件目录并重启 Listary。";
const wchar_t* kNoListaryHintText = L"未检测到 Listary，此项不可用。";

const wchar_t* kShellWindowCheckText = L"允许其它程序定位到我打开的文件（实验性）";
const wchar_t* kShellWindowHintText  = L"注册为 Shell 窗口；Listary 等工具能读到当前目录。";

const wchar_t* kActionFailedText = L"上次操作失败，详情见日志文件。";

} // 匿名命名空间

SettingsDialog::~SettingsDialog()
{
    if (m_font != nullptr)
    {
        ::DeleteObject(m_font);
    }
    if (m_fontTitle != nullptr)
    {
        ::DeleteObject(m_fontTitle);
    }
}

bool SettingsDialog::EnsureClassRegistered()
{
    static bool registered = false;
    if (registered)
    {
        return true;
    }

    if (!RegisterWindowClass(kClassName, ::GetModuleHandleW(nullptr), CS_HREDRAW | CS_VREDRAW))
    {
        return false;
    }

    registered = true;
    return true;
}

bool SettingsDialog::ShowModal(HWND owner)
{
    m_hOwner = owner;
    SetDpiSource(owner);

    if (!CreateDialogWindow())
    {
        return false;
    }

    RunModalLoop(m_hOwner);
    return true;
}

bool SettingsDialog::CreateDialogWindow()
{
    if (!EnsureClassRegistered())
    {
        return false;
    }

    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);

    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
    {
        m_font = ::CreateFontIndirectW(&metrics.lfMessageFont);

        LOGFONTW titleFont = metrics.lfMessageFont;
        titleFont.lfWeight = FW_SEMIBOLD;
        titleFont.lfHeight = static_cast<LONG>(titleFont.lfHeight * 1.25);
        m_fontTitle = ::CreateFontIndirectW(&titleFont);
    }

    if (m_font == nullptr)
    {
        m_font = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    }
    if (m_fontTitle == nullptr)
    {
        m_fontTitle = m_font;
    }

    int dpi = ::GetDpiForWindow(m_hOwner != nullptr ? m_hOwner : ::GetDesktopWindow());
    if (dpi <= 0)
    {
        dpi = 96;
    }

    RECT windowRect = { 0, 0, ::MulDiv(kClientWidth, dpi, 96), ::MulDiv(kClientHeight, dpi, 96) };
    ::AdjustWindowRectEx(&windowRect, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE,
                         WS_EX_DLGMODALFRAME);

    const int width  = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;

    RECT ownerRect = {};
    if (m_hOwner != nullptr)
    {
        ::GetWindowRect(m_hOwner, &ownerRect);
    }

    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top  + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    // 以主窗口为中心可能把底部推到屏幕/任务栏外面，夹到工作区内
    RECT workArea = {};
    if (::SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0))
    {
        int left = x;
        int top  = y;

        if (left + width > workArea.right)  { left = workArea.right - width; }
        if (top + height > workArea.bottom) { top = workArea.bottom - height; }
        if (left < workArea.left)           { left = workArea.left; }
        if (top < workArea.top)             { top = workArea.top; }

        m_hWnd = ::CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                                   kClassName,
                                   L"设置",
                                   WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                   left, top, width, height,
                                   m_hOwner,
                                   nullptr,
                                   ::GetModuleHandleW(nullptr),
                                   this);
    }
    else
    {
        m_hWnd = ::CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                                   kClassName,
                                   L"设置",
                                   WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                   x, y, width, height,
                                   m_hOwner,
                                   nullptr,
                                   ::GetModuleHandleW(nullptr),
                                   this);
    }

    if (m_hWnd == nullptr)
    {
        Logger::Error(L"设置窗口创建失败");
        return false;
    }

    auto createCheck = [this](const wchar_t* text, int id, DWORD style) -> HWND
    {
        HWND check = ::CreateWindowExW(0, L"BUTTON", text,
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                                       0, 0, 0, 0, m_hWnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                       ::GetModuleHandleW(nullptr), nullptr);
        if (check != nullptr)
        {
            ::SendMessageW(check, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        }
        return check;
    };

    // ---- 查看选项 ----
    m_hCheck       = createCheck(L"右键菜单显示图标", kControlIdMenuIcons, BS_AUTOCHECKBOX);
    m_hCheckHidden = createCheck(kHiddenCheckText, kControlIdShowHidden, BS_AUTOCHECKBOX);
    m_hCheckSystem = createCheck(kSystemCheckText, kControlIdShowSystem, BS_AUTOCHECKBOX);

    SetChecked(m_hCheck, core::Settings::Instance().MenuIcons());
    SetChecked(m_hCheckHidden, core::Settings::Instance().ShowHiddenFiles());
    SetChecked(m_hCheckSystem, core::Settings::Instance().ShowSystemFiles());

    // ---- 启动位置：4 个互斥单选 ----
    m_hRadioThisPc = createCheck(kRadioThisPcText, kControlIdRadioThisPc,
                                 BS_AUTORADIOBUTTON | WS_GROUP);
    m_hRadioHome   = createCheck(kRadioHomeText, kControlIdRadioHome, BS_AUTORADIOBUTTON);
    m_hRadioCustom = createCheck(kRadioCustomText, kControlIdRadioCustom, BS_AUTORADIOBUTTON);
    m_hRadioLast   = createCheck(kRadioLastText, kControlIdRadioLast, BS_AUTORADIOBUTTON);

    // ---- 快速启动 ----
    m_hCheckBackground = createCheck(kBackgroundCheckText, kControlIdBackground, BS_AUTOCHECKBOX);
    m_hCheckWinE       = createCheck(kWinECheckText, kControlIdWinE, BS_AUTOCHECKBOX);

    // ---- 注册与集成 ----
    m_hCheckTabs           = createCheck(kTabsCheckText, kControlIdTabs, BS_AUTOCHECKBOX);
    m_hCheckSingleInstance = createCheck(kSingleInstanceCheckText, kControlIdSingleInstance, BS_AUTOCHECKBOX);

    m_hCheckDefaultManager = createCheck(kDefaultCheckText, kControlIdDefaultManager, BS_AUTOCHECKBOX);
    m_hCheckProxy          = createCheck(kProxyCheckText, kControlIdProxy, BS_AUTOCHECKBOX);
    m_hCheckListary        = createCheck(kListaryCheckText, kControlIdListary, BS_AUTOCHECKBOX);
    m_hCheckShellWindow    = createCheck(kShellWindowCheckText, kControlIdShellWindow, BS_AUTOCHECKBOX);

    RefreshStartupControls();
    RefreshQuickStart();
    RefreshWindowModeChecks();
    RefreshRegistrationChecks();

    Layout();
    return true;
}

void SettingsDialog::Layout()
{
    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);

    const int padding = Scale(kPadding);
    const int width   = clientRect.right - padding * 2;
    const int height  = Scale(24);

    auto place = [&](HWND control, int y)
    {
        if (control != nullptr)
        {
            ::MoveWindow(control, padding, padding + Scale(y), width, height, TRUE);
        }
    };

    // 查看选项
    place(m_hCheck,       44);
    place(m_hCheckHidden, 118);
    place(m_hCheckSystem, 146);

    // 启动位置：2x2 摆放
    const int halfWidth = (width - Scale(12)) / 2;
    auto placeAt = [&](HWND control, int x, int y, int w)
    {
        if (control != nullptr)
        {
            ::MoveWindow(control, padding + Scale(x), padding + Scale(y),
                         w, height, TRUE);
        }
    };

    placeAt(m_hRadioThisPc, 0,   224, halfWidth);
    placeAt(m_hRadioHome,   190, 224, halfWidth);
    placeAt(m_hRadioCustom, 0,   248, halfWidth);
    placeAt(m_hRadioLast,   190, 248, halfWidth);

    // 「浏览...」按钮
    const int browseWidth  = Scale(80);
    const int browseHeight = Scale(28);
    m_browseRect.left   = clientRect.right - padding - browseWidth;
    m_browseRect.top    = padding + Scale(266);
    m_browseRect.right  = m_browseRect.left + browseWidth;
    m_browseRect.bottom = m_browseRect.top + browseHeight;

    // 快速启动
    place(m_hCheckBackground, 334);
    place(m_hCheckWinE,       374);

    // 窗口模式
    place(m_hCheckTabs,           424);
    place(m_hCheckSingleInstance, 458);

    // 注册与集成
    place(m_hCheckDefaultManager, 516);
    place(m_hCheckProxy,          560);
    place(m_hCheckListary,        604);
    place(m_hCheckShellWindow,    648);

    // 关闭按钮
    const int buttonWidth  = Scale(88);
    const int buttonHeight = Scale(30);
    m_closeRect.left   = clientRect.right - padding - buttonWidth;
    m_closeRect.top    = clientRect.bottom - padding - buttonHeight;
    m_closeRect.right  = m_closeRect.left + buttonWidth;
    m_closeRect.bottom = m_closeRect.top + buttonHeight;
}

bool SettingsDialog::IsChecked(HWND check)
{
    return check != nullptr && ::SendMessageW(check, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void SettingsDialog::SetChecked(HWND check, bool checked)
{
    if (check != nullptr)
    {
        ::SendMessageW(check, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

void SettingsDialog::ApplyCheckState()
{
    if (m_hCheck == nullptr)
    {
        return;
    }

    const bool showIcons  = IsChecked(m_hCheck);
    const bool showHidden = IsChecked(m_hCheckHidden);
    const bool showSystem = IsChecked(m_hCheckSystem);
    const bool shellWindow = IsChecked(m_hCheckShellWindow);

    core::Settings& settings = core::Settings::Instance();
    settings.SetMenuIcons(showIcons);
    settings.SetShowHiddenFiles(showHidden);
    settings.SetShowSystemFiles(showSystem);
    settings.SetShellWindowIntegration(shellWindow);
    settings.Save();

    Logger::Info(std::wstring(L"设置：右键菜单显示图标 = ") + (showIcons ? L"开" : L"关") +
                 L"，显示隐藏文件 = " + (showHidden ? L"开" : L"关") +
                 L"，显示系统文件 = " + (showSystem ? L"开" : L"关") +
                 L"，Shell 窗口集成 = " + (shellWindow ? L"开" : L"关"));
}

void SettingsDialog::ApplyStartupLocation()
{
    core::Settings& settings = core::Settings::Instance();

    if (IsChecked(m_hRadioThisPc))
    {
        settings.SetStartup(core::StartupLocation::ThisPc);
    }
    else if (IsChecked(m_hRadioHome))
    {
        settings.SetStartup(core::StartupLocation::Home);
    }
    else if (IsChecked(m_hRadioCustom))
    {
        settings.SetStartup(core::StartupLocation::Custom);
    }
    else
    {
        settings.SetStartup(core::StartupLocation::LastFolder);
    }

    settings.Save();
    ui::ReloadBackgroundHost();

    Logger::Info(L"设置：启动位置 = " + std::to_wstring(static_cast<int>(settings.Startup())));
    ::InvalidateRect(m_hWnd, nullptr, TRUE);
}

void SettingsDialog::RefreshStartupControls()
{
    const core::StartupLocation location = core::Settings::Instance().Startup();
    SetChecked(m_hRadioThisPc, location == core::StartupLocation::ThisPc);
    SetChecked(m_hRadioHome,   location == core::StartupLocation::Home);
    SetChecked(m_hRadioCustom, location == core::StartupLocation::Custom);
    SetChecked(m_hRadioLast,   location == core::StartupLocation::LastFolder);
}

void SettingsDialog::BrowseCustomFolder()
{
    IFileDialog* dialog = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog))))
    {
        return;
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options)))
    {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    }
    dialog->SetTitle(L"选择启动时打开的文件夹");

    if (SUCCEEDED(dialog->Show(m_hWnd)))
    {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr)
        {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr)
            {
                core::Settings& settings = core::Settings::Instance();
                settings.SetCustomStartupFolder(path);
                settings.SetStartup(core::StartupLocation::Custom);
                settings.Save();

                ::CoTaskMemFree(path);
                RefreshStartupControls();
                ui::ReloadBackgroundHost();
                ::InvalidateRect(m_hWnd, nullptr, TRUE);
            }
            item->Release();
        }
    }

    dialog->Release();
}

void SettingsDialog::OnQuickStartToggle(int controlId)
{
    core::Settings& settings = core::Settings::Instance();

    if (controlId == kControlIdBackground)
    {
        const bool want = IsChecked(m_hCheckBackground);
        settings.SetBackgroundStartup(want);
        if (!want)
        {
            settings.SetTakeOverWinE(false);
        }
        settings.Save();   // 先落盘，后台进程启动时才能读到新值

        if (want)
        {
            shell::SetAutoStart(true);
            ui::StartBackgroundHost();
            ui::ReloadBackgroundHost();
        }
        else
        {
            shell::SetAutoStart(false);
            ui::StopBackgroundHost();
        }
    }
    else if (controlId == kControlIdWinE)
    {
        const bool want = IsChecked(m_hCheckWinE);
        settings.SetTakeOverWinE(want);
        if (want)
        {
            // 勾 Win+E 自动勾后台启动
            settings.SetBackgroundStartup(true);
        }
        settings.Save();   // 先落盘，后台进程启动时才能读到新值

        if (want)
        {
            shell::SetAutoStart(true);
            ui::StartBackgroundHost();
        }
        ui::ReloadBackgroundHost();
    }

    RefreshQuickStart();
    ::InvalidateRect(m_hWnd, nullptr, TRUE);
}

void SettingsDialog::RefreshQuickStart()
{
    const core::Settings& settings = core::Settings::Instance();
    SetChecked(m_hCheckBackground, settings.BackgroundStartup());
    SetChecked(m_hCheckWinE, settings.TakeOverWinE());
}

void SettingsDialog::RefreshWindowModeChecks()
{
    const core::Settings& settings = core::Settings::Instance();
    SetChecked(m_hCheckTabs, settings.TabsEnabled());
    SetChecked(m_hCheckSingleInstance, settings.SingleInstance());
}

void SettingsDialog::OnRegistrationToggle(int controlId)
{
    const bool want = IsChecked(
        controlId == kControlIdDefaultManager ? m_hCheckDefaultManager :
        controlId == kControlIdProxy          ? m_hCheckProxy :
        controlId == kControlIdListary        ? m_hCheckListary :
                                                m_hCheckShellWindow);

    bool ok = true;

    switch (controlId)
    {
    case kControlIdDefaultManager:
        ok = want ? shell::RegisterDefaultFileManager()
                  : shell::RestoreSystemFileManager();
        break;

    case kControlIdProxy:
        ok = want ? shell::DeployProxyIfNeeded(m_hWnd)
                  : shell::RemoveProxyIfOurs(m_hWnd);
        break;

    case kControlIdListary:
        if (shell::IsListaryInstalled())
        {
            ok = want ? shell::InstallListaryPluginElevated(m_hWnd)
                      : shell::RemoveListaryPluginElevated(m_hWnd);
        }
        break;

    default:
        break;
    }

    m_lastActionFailed = !ok;
    RefreshRegistrationChecks();
    ::InvalidateRect(m_hWnd, nullptr, TRUE);
}

void SettingsDialog::RefreshRegistrationChecks()
{
    SetChecked(m_hCheckDefaultManager, shell::IsDefaultFileManager());
    SetChecked(m_hCheckProxy, shell::QuerySystemProxyState() == shell::ProxyState::OursUpToDate);
    SetChecked(m_hCheckListary, shell::IsListaryPluginInstalled());
    SetChecked(m_hCheckShellWindow, core::Settings::Instance().ShellWindowIntegration());

    if (m_hCheckListary != nullptr)
    {
        ::EnableWindow(m_hCheckListary, shell::IsListaryInstalled() ? TRUE : FALSE);
    }
}

void SettingsDialog::Draw(HDC dc)
{
    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);

    HBRUSH background = ::CreateSolidBrush(kBackgroundColor);
    ::FillRect(dc, &clientRect, background);
    ::DeleteObject(background);

    ::SetBkMode(dc, TRANSPARENT);

    // ---- 标题 ----
    RECT titleRect = { Scale(kPadding), Scale(kPadding), clientRect.right - Scale(kPadding),
                       Scale(kPadding) + Scale(30) };

    HGDIOBJ oldFont = ::SelectObject(dc, m_fontTitle);
    ::SetTextColor(dc, kTitleColor);
    ::DrawTextW(dc, kTitleText, -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // ---- 查看选项说明 ----
    ::SelectObject(dc, m_font);
    ::SetTextColor(dc, kHintColor);

    RECT hintRect = { Scale(kPadding), Scale(kPadding) + Scale(66),
                      clientRect.right - Scale(kPadding), Scale(kPadding) + Scale(110) };

    ::DrawTextW(dc, kHintText1, -1, &hintRect, DT_LEFT | DT_TOP | DT_WORDBREAK);
    hintRect.top += Scale(18);
    ::DrawTextW(dc, kHintText2, -1, &hintRect, DT_LEFT | DT_TOP | DT_WORDBREAK);

    RECT hiddenHintRect = { Scale(kPadding), Scale(kPadding) + Scale(170),
                            clientRect.right - Scale(kPadding), Scale(kPadding) + Scale(190) };
    ::DrawTextW(dc, kHiddenHintText, -1, &hiddenHintRect, DT_LEFT | DT_TOP | DT_SINGLELINE);

    // 分隔线 1
    const int divider1 = Scale(kPadding) + Scale(192);
    HPEN dividerPen = ::CreatePen(PS_SOLID, 1, kDividerColor);
    HGDIOBJ oldPen  = ::SelectObject(dc, dividerPen);
    ::MoveToEx(dc, Scale(kPadding), divider1, nullptr);
    ::LineTo(dc, clientRect.right - Scale(kPadding), divider1);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(dividerPen);

    // ---- 启动位置 ----
    ::SetTextColor(dc, kTitleColor);
    RECT startupTitle = { Scale(kPadding), Scale(kPadding) + Scale(198),
                          clientRect.right - Scale(kPadding), Scale(kPadding) + Scale(220) };
    ::DrawTextW(dc, kStartupTitle, -1, &startupTitle, DT_LEFT | DT_TOP | DT_SINGLELINE);

    // 自定义路径
    const core::Settings& settings = core::Settings::Instance();
    std::wstring path = settings.CustomStartupFolder();
    if (path.empty())
    {
        path = L"（未选择）";
    }

    RECT pathRect = { Scale(kPadding) + Scale(24), m_browseRect.top + Scale(2),
                      m_browseRect.left - Scale(8), m_browseRect.bottom };
    ::SetTextColor(dc, path == L"（未选择）" ? kHintColor : kTitleColor);
    ::DrawTextW(dc, path.c_str(), -1, &pathRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);

    DrawButton(dc, m_browseRect, L"浏览...", m_hotBrowse, m_pressedBrowse);

    // 分隔线 2
    const int divider2 = Scale(kPadding) + Scale(302);
    dividerPen = ::CreatePen(PS_SOLID, 1, kDividerColor);
    oldPen  = ::SelectObject(dc, dividerPen);
    ::MoveToEx(dc, Scale(kPadding), divider2, nullptr);
    ::LineTo(dc, clientRect.right - Scale(kPadding), divider2);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(dividerPen);

    // ---- 快速启动 ----
    ::SetTextColor(dc, kTitleColor);
    RECT quickTitle = { Scale(kPadding), Scale(kPadding) + Scale(310),
                        clientRect.right - Scale(kPadding), Scale(kPadding) + Scale(332) };
    ::DrawTextW(dc, kQuickTitle, -1, &quickTitle, DT_LEFT | DT_TOP | DT_SINGLELINE);

    // 分隔线 3（快速启动 / 窗口模式）
    const int divider3 = Scale(kPadding) + Scale(402);
    dividerPen = ::CreatePen(PS_SOLID, 1, kDividerColor);
    oldPen  = ::SelectObject(dc, dividerPen);
    ::MoveToEx(dc, Scale(kPadding), divider3, nullptr);
    ::LineTo(dc, clientRect.right - Scale(kPadding), divider3);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(dividerPen);

    // ---- 窗口模式 ----
    ::SetTextColor(dc, kTitleColor);
    RECT windowTitle = { Scale(kPadding), Scale(kPadding) + Scale(408),
                         clientRect.right - Scale(kPadding), Scale(kPadding) + Scale(428) };
    ::DrawTextW(dc, kWindowTitle, -1, &windowTitle, DT_LEFT | DT_TOP | DT_SINGLELINE);

    // 分隔线 4（窗口模式 / 注册与集成）
    const int divider4 = Scale(kPadding) + Scale(484);
    dividerPen = ::CreatePen(PS_SOLID, 1, kDividerColor);
    oldPen  = ::SelectObject(dc, dividerPen);
    ::MoveToEx(dc, Scale(kPadding), divider4, nullptr);
    ::LineTo(dc, clientRect.right - Scale(kPadding), divider4);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(dividerPen);

    // ---- 注册与集成 ----
    ::SetTextColor(dc, kTitleColor);
    RECT sectionRect = { Scale(kPadding), Scale(kPadding) + Scale(492),
                         clientRect.right - Scale(kPadding), Scale(kPadding) + Scale(514) };
    ::DrawTextW(dc, kSectionTitle, -1, &sectionRect, DT_LEFT | DT_TOP | DT_SINGLELINE);

    ::SetTextColor(dc, kHintColor);
    RECT regHint = { Scale(kPadding), Scale(kPadding) + Scale(672),
                     clientRect.right - Scale(kPadding), Scale(kPadding) + Scale(692) };
    ::DrawTextW(dc, L"代理和 Listary 插件需要管理员权限；失败详情见日志文件。", -1, &regHint,
                DT_LEFT | DT_TOP | DT_SINGLELINE);

    if (m_lastActionFailed)
    {
        RECT errorRect = { Scale(kPadding), Scale(kPadding) + Scale(696),
                           clientRect.right - Scale(kPadding), Scale(kPadding) + Scale(716) };
        ::SetTextColor(dc, kErrorColor);
        ::DrawTextW(dc, kActionFailedText, -1, &errorRect, DT_LEFT | DT_TOP | DT_SINGLELINE);
    }

    ::SelectObject(dc, oldFont);
    DrawButton(dc, m_closeRect, L"关闭", m_hotClose, m_pressedClose);
}

void SettingsDialog::DrawButton(HDC dc, const RECT& rect, const wchar_t* text,
                                bool hot, bool pressed)
{
    ui::FlatButtonVisual visual;
    visual.hot     = hot;
    visual.pressed = pressed;

    ui::DrawFlatButton(dc, rect, text, m_font, visual);
}

LRESULT SettingsDialog::HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_COMMAND:
    {
        const int controlId = LOWORD(wParam);
        const int notifyCode = HIWORD(wParam);

        if (notifyCode != BN_CLICKED)
        {
            break;
        }

        if (controlId == kControlIdMenuIcons ||
            controlId == kControlIdShowHidden ||
            controlId == kControlIdShowSystem ||
            controlId == kControlIdShellWindow)
        {
            ApplyCheckState();
            return 0;
        }

        if (controlId == kControlIdRadioThisPc ||
            controlId == kControlIdRadioHome ||
            controlId == kControlIdRadioCustom ||
            controlId == kControlIdRadioLast)
        {
            ApplyStartupLocation();
            return 0;
        }

        if (controlId == kControlIdBackground || controlId == kControlIdWinE)
        {
            OnQuickStartToggle(controlId);
            return 0;
        }

        if (controlId == kControlIdTabs || controlId == kControlIdSingleInstance)
        {
            core::Settings& settings = core::Settings::Instance();
            if (controlId == kControlIdTabs)
            {
                settings.SetTabsEnabled(IsChecked(m_hCheckTabs));
            }
            else
            {
                settings.SetSingleInstance(IsChecked(m_hCheckSingleInstance));
            }
            settings.Save();
            RefreshWindowModeChecks();
            return 0;
        }

        if (controlId == kControlIdDefaultManager ||
            controlId == kControlIdProxy ||
            controlId == kControlIdListary)
        {
            OnRegistrationToggle(controlId);
            return 0;
        }

        if (controlId == kControlIdClose)
        {
            ::DestroyWindow(hWnd);
            return 0;
        }
        break;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT paint = {};
        HDC dc = ::BeginPaint(hWnd, &paint);
        Draw(dc);
        ::EndPaint(hWnd, &paint);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        const bool hotClose  = (::PtInRect(&m_closeRect, pt) != FALSE);
        const bool hotBrowse = (::PtInRect(&m_browseRect, pt) != FALSE);

        if (hotClose != m_hotClose)
        {
            m_hotClose = hotClose;
            ::InvalidateRect(hWnd, &m_closeRect, FALSE);
        }
        if (hotBrowse != m_hotBrowse)
        {
            m_hotBrowse = hotBrowse;
            ::InvalidateRect(hWnd, &m_browseRect, FALSE);
        }

        if (!m_trackingLeave)
        {
            TRACKMOUSEEVENT track = {};
            track.cbSize    = sizeof(track);
            track.dwFlags   = TME_LEAVE;
            track.hwndTrack = hWnd;
            ::TrackMouseEvent(&track);
            m_trackingLeave = true;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        m_trackingLeave = false;

        if (m_hotClose || m_pressedClose)
        {
            m_hotClose     = false;
            m_pressedClose = false;
            ::InvalidateRect(hWnd, &m_closeRect, FALSE);
        }
        if (m_hotBrowse || m_pressedBrowse)
        {
            m_hotBrowse     = false;
            m_pressedBrowse = false;
            ::InvalidateRect(hWnd, &m_browseRect, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        if (::PtInRect(&m_closeRect, pt))
        {
            m_pressedClose = true;
            ::SetCapture(hWnd);
            ::InvalidateRect(hWnd, &m_closeRect, FALSE);
            return 0;
        }
        if (::PtInRect(&m_browseRect, pt))
        {
            m_pressedBrowse = true;
            ::SetCapture(hWnd);
            ::InvalidateRect(hWnd, &m_browseRect, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        if (m_pressedClose)
        {
            m_pressedClose = false;
            ::ReleaseCapture();
            const bool inside = (::PtInRect(&m_closeRect, pt) != FALSE);
            ::InvalidateRect(hWnd, &m_closeRect, FALSE);
            if (inside)
            {
                ::DestroyWindow(hWnd);
            }
            return 0;
        }

        if (m_pressedBrowse)
        {
            m_pressedBrowse = false;
            ::ReleaseCapture();
            const bool inside = (::PtInRect(&m_browseRect, pt) != FALSE);
            ::InvalidateRect(hWnd, &m_browseRect, FALSE);
            if (inside)
            {
                BrowseCustomFolder();
            }
        }
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
        m_hWnd   = nullptr;
        m_hCheck = nullptr;
        m_hCheckHidden = nullptr;
        m_hCheckSystem = nullptr;
        m_hRadioThisPc = nullptr;
        m_hRadioHome = nullptr;
        m_hRadioCustom = nullptr;
        m_hRadioLast = nullptr;
        m_hCheckBackground = nullptr;
        m_hCheckWinE = nullptr;
        m_hCheckTabs = nullptr;
        m_hCheckSingleInstance = nullptr;
        m_hCheckDefaultManager = nullptr;
        m_hCheckProxy = nullptr;
        m_hCheckListary = nullptr;
        m_hCheckShellWindow = nullptr;
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
