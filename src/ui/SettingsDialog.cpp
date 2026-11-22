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
#include <uxtheme.h>

#include "ui/SettingsDialog.h"

#pragma comment(lib, "uxtheme.lib")

#include "core/Logger.h"
#include "core/Settings.h"
#include "shell/AutoStart.h"
#include "shell/DefaultManager.h"
#include "shell/ExplorerProxy.h"
#include "shell/ListaryIntegration.h"
#include "shell/TraceCleanup.h"
#include "ui/BackgroundHost.h"
#include "ui/FlatDraw.h"

namespace
{

const wchar_t* kClassName = L"FileExplorerSettingsDialog";

const int kClientWidth  = 460;
const int kClientHeight = 840;
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
const int kControlIdStartupCombo   = 4018;
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

const wchar_t* kStartupTitle = L"启动位置";
const wchar_t* kRadioThisPcText = L"此电脑";
const wchar_t* kRadioHomeText   = L"主文件夹";
const wchar_t* kRadioCustomText = L"自定义";
const wchar_t* kRadioLastText   = L"最后一次打开的文件夹";
const wchar_t* kStartupOptions[] = {
    kRadioLastText, kRadioHomeText, kRadioThisPcText, kRadioCustomText
};
const wchar_t* kStartupHintText = L"自定义目录不存在时回退到主文件夹；显式带路径启动不受影响。";

const wchar_t* kQuickTitle = L"快速启动";
const wchar_t* kBackgroundCheckText = L"开机时后台启动（托盘常驻，关闭窗口不退出）";
const wchar_t* kBackgroundHintText  = L"登录时启动一个隐藏窗口，之后打开会快很多。";
const wchar_t* kWinECheckText = L"接管 Win+E（勾选时自动开启上面的后台启动）";
const wchar_t* kWinEHintText  = L"按 Win+E 新开一个窗口，停在上面选择的位置。";

const wchar_t* kWindowTitle = L"窗口模式";
const wchar_t* kTabsCheckText = L"多标签模式（关掉就是一个窗口一个目录）";
const wchar_t* kSingleInstanceCheckText = L"只允许一个实例（所有打开都进现有窗口）";

const wchar_t* kSectionTitle = L"注册与集成";
const wchar_t* kViewTitle = L"查看";
const wchar_t* kQuickTitleShort = L"快速启动";
const wchar_t* kWindowTitleShort = L"窗口";
const wchar_t* kSubtitleText = L"外观、启动与系统集成";

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

// 自绘复选框：保留 BS_AUTOCHECKBOX 的点击/状态行为，只替换外观。
LRESULT CALLBACK SettingsCheckboxSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                              LPARAM lParam, UINT_PTR idSubclass,
                                              DWORD_PTR refData)
{
    (void)idSubclass;
    (void)refData;

    if (msg == WM_ERASEBKGND)
    {
        return 1;
    }

    if (msg == WM_PAINT)
    {
        PAINTSTRUCT ps = {};
        HDC dc = ::BeginPaint(hwnd, &ps);

        RECT rc = {};
        ::GetClientRect(hwnd, &rc);

        HBRUSH background = ::CreateSolidBrush(RGB(255, 255, 255));
        ::FillRect(dc, &rc, background);
        ::DeleteObject(background);

        const int dpi = ::GetDpiForWindow(hwnd);
        const int boxSize = ::MulDiv(16, dpi > 0 ? dpi : 96, 96);
        const int boxLeft = ::MulDiv(4, dpi > 0 ? dpi : 96, 96);
        const int boxTop = (rc.bottom - boxSize) / 2;

        RECT box = { boxLeft, boxTop, boxLeft + boxSize, boxTop + boxSize };
        const bool checked = (::SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED);
        const bool enabled = ::IsWindowEnabled(hwnd) != FALSE;

        const COLORREF accent = enabled ? RGB(0, 103, 192) : RGB(190, 195, 201);
        const COLORREF boxFill = checked ? accent : RGB(255, 255, 255);
        const COLORREF boxBorder = checked ? accent
                                           : (enabled ? RGB(185, 190, 197) : RGB(214, 217, 222));

        HBRUSH fillBrush = ::CreateSolidBrush(boxFill);
        HPEN borderPen = ::CreatePen(PS_SOLID, 1, boxBorder);
        HGDIOBJ oldBrush = ::SelectObject(dc, fillBrush);
        HGDIOBJ oldPen = ::SelectObject(dc, borderPen);
        ::RoundRect(dc, box.left, box.top, box.right, box.bottom,
                    ::MulDiv(4, dpi > 0 ? dpi : 96, 96),
                    ::MulDiv(4, dpi > 0 ? dpi : 96, 96));
        ::SelectObject(dc, oldBrush);
        ::SelectObject(dc, oldPen);
        ::DeleteObject(fillBrush);
        ::DeleteObject(borderPen);

        if (checked)
        {
            HPEN checkPen = ::CreatePen(PS_SOLID, ::MulDiv(2, dpi > 0 ? dpi : 96, 96),
                                        RGB(255, 255, 255));
            HGDIOBJ old = ::SelectObject(dc, checkPen);
            ::MoveToEx(dc, box.left + boxSize / 4, box.top + boxSize / 2, nullptr);
            ::LineTo(dc, box.left + boxSize / 2 - ::MulDiv(1, dpi > 0 ? dpi : 96, 96),
                     box.bottom - boxSize / 4);
            ::LineTo(dc, box.right - boxSize / 4, box.top + boxSize / 4);
            ::SelectObject(dc, old);
            ::DeleteObject(checkPen);
        }

        wchar_t text[512] = {};
        ::GetWindowTextW(hwnd, text, ARRAYSIZE(text));

        RECT textRect = rc;
        textRect.left = box.right + ::MulDiv(8, dpi > 0 ? dpi : 96, 96);
        textRect.right -= ::MulDiv(4, dpi > 0 ? dpi : 96, 96);

        HFONT font = reinterpret_cast<HFONT>(::SendMessageW(hwnd, WM_GETFONT, 0, 0));
        HGDIOBJ oldFont = ::SelectObject(dc, font != nullptr ? font
                                                             : ::GetStockObject(DEFAULT_GUI_FONT));
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, enabled ? RGB(42, 46, 52) : RGB(160, 164, 170));
        ::DrawTextW(dc, text, -1, &textRect,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        ::SelectObject(dc, oldFont);

        ::EndPaint(hwnd, &ps);
        return 0;
    }

    if (msg == WM_ENABLE || msg == WM_SETFOCUS || msg == WM_KILLFOCUS)
    {
        ::InvalidateRect(hwnd, nullptr, FALSE);
    }

    return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}



} // 匿名命名空间

std::wstring StartupCustomDisplayName(const std::wstring& path)
{
    if (path.empty())
    {
        return path;
    }

    // 普通磁盘目录直接显示路径；虚拟位置（此电脑/主文件夹）显示系统名
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        return path;
    }

    IShellItem* item = nullptr;
    if (SUCCEEDED(::SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) &&
        item != nullptr)
    {
        PWSTR name = nullptr;
        std::wstring result = path;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name != nullptr)
        {
            result.assign(name);
            ::CoTaskMemFree(name);
        }
        item->Release();
        return result;
    }

    return path;
}

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
    if (m_fontSection != nullptr)
    {
        ::DeleteObject(m_fontSection);
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

        LOGFONTW sectionFont = metrics.lfMessageFont;
        sectionFont.lfWeight = FW_SEMIBOLD;
        m_fontSection = ::CreateFontIndirectW(&sectionFont);
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
            ::SetWindowSubclass(check, &SettingsCheckboxSubclassProc, 1, 0);
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

    // ---- 启动位置：下拉框 ----
    m_hStartupCombo = ::CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 0, 0, m_hWnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlIdStartupCombo)),
        ::GetModuleHandleW(nullptr), nullptr);

    if (m_hStartupCombo != nullptr)
    {
        ::SendMessageW(m_hStartupCombo, WM_SETFONT,
                       reinterpret_cast<WPARAM>(m_font), TRUE);
        ::SetWindowTheme(m_hStartupCombo, L"Explorer", nullptr);

        for (const wchar_t* option : kStartupOptions)
        {
            ::SendMessageW(m_hStartupCombo, CB_ADDSTRING, 0,
                           reinterpret_cast<LPARAM>(option));
        }
    }

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
    const int width = clientRect.right - padding * 2;
    const int rowHeight = Scale(24);

    auto place = [&](HWND control, int y)
    {
        if (control != nullptr)
        {
            ::MoveWindow(control, padding, Scale(y), width, rowHeight, TRUE);
        }
    };

    // 查看
    place(m_hCheck,       112);
    place(m_hCheckHidden, 146);
    place(m_hCheckSystem, 180);

    // 启动位置下拉框（给下拉列表留足高度）
    if (m_hStartupCombo != nullptr)
    {
        ::MoveWindow(m_hStartupCombo, padding, Scale(292),
                     width, Scale(200), TRUE);
    }

    // 快速启动
    place(m_hCheckBackground, 388);
    place(m_hCheckWinE,       422);

    // 窗口
    place(m_hCheckTabs,           528);
    place(m_hCheckSingleInstance, 562);

    // 注册与集成
    place(m_hCheckDefaultManager, 626);
    place(m_hCheckProxy,          656);
    place(m_hCheckListary,        686);
    place(m_hCheckShellWindow,    716);

    // 底部按钮：恢复/重置（左）+ 关闭（右）
    const int closeWidth   = Scale(88);
    const int restoreWidth = Scale(100);
    const int buttonHeight = Scale(30);
    const int buttonGap    = Scale(12);

    m_closeRect.left = clientRect.right - padding - closeWidth;
    m_closeRect.top = clientRect.bottom - padding - buttonHeight;
    m_closeRect.right = m_closeRect.left + closeWidth;
    m_closeRect.bottom = m_closeRect.top + buttonHeight;

    m_restoreRect.right = m_closeRect.left - buttonGap;
    m_restoreRect.left  = m_restoreRect.right - restoreWidth;
    m_restoreRect.top    = m_closeRect.top;
    m_restoreRect.bottom = m_closeRect.bottom;
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
    if (m_hStartupCombo == nullptr)
    {
        return;
    }

    const int sel = static_cast<int>(::SendMessageW(m_hStartupCombo, CB_GETCURSEL, 0, 0));
    const core::StartupLocation order[] = {
        core::StartupLocation::LastFolder,
        core::StartupLocation::Home,
        core::StartupLocation::ThisPc,
        core::StartupLocation::Custom,
    };

    if (sel < 0 || sel >= static_cast<int>(sizeof(order) / sizeof(order[0])) ||
        sel == 3)
    {
        return; // 自定义由 PickCustomStartupFolder 处理
    }

    core::Settings& settings = core::Settings::Instance();
    settings.SetStartup(order[sel]);
    settings.Save();
    ui::ReloadBackgroundHost();

    Logger::Info(L"设置：启动位置 = " + std::to_wstring(static_cast<int>(settings.Startup())));
    RefreshStartupControls();
    ::InvalidateRect(m_hWnd, nullptr, TRUE);
}

void SettingsDialog::PickCustomStartupFolder()
{
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog))) ||
        dialog == nullptr)
    {
        RefreshStartupControls();
        return;
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options)))
    {
        // 不强制文件系统，允许选择“此电脑”“主文件夹”等 Shell 位置
        dialog->SetOptions((options | FOS_PICKFOLDERS) & ~FOS_FORCEFILESYSTEM);
    }
    dialog->SetTitle(L"选择自定义启动位置");

    const core::Settings& settings = core::Settings::Instance();
    const std::wstring current = settings.CustomStartupFolder();
    if (!current.empty())
    {
        IShellItem* currentItem = nullptr;
        if (SUCCEEDED(::SHCreateItemFromParsingName(current.c_str(), nullptr,
                                                   IID_PPV_ARGS(&currentItem))) &&
            currentItem != nullptr)
        {
            dialog->SetFolder(currentItem);   // 下次打开直接定位到上次选择
            currentItem->Release();
        }
    }

    const HRESULT shown = dialog->Show(m_hWnd);
    if (FAILED(shown))
    {
        dialog->Release();
        RefreshStartupControls();   // 取消选择：恢复原下拉项
        return;
    }

    std::wstring chosen;

    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr)
    {
        PWSTR value = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &value)) && value != nullptr)
        {
            chosen.assign(value);
            ::CoTaskMemFree(value);
        }
        else
        {
            value = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &value)) &&
                value != nullptr)
            {
                chosen.assign(value);
                ::CoTaskMemFree(value);
            }
        }

        item->Release();
    }
    dialog->Release();

    if (!chosen.empty())
    {
        core::Settings& mutableSettings = core::Settings::Instance();
        mutableSettings.SetCustomStartupFolder(chosen);
        mutableSettings.SetStartup(core::StartupLocation::Custom);
        mutableSettings.Save();
        ui::ReloadBackgroundHost();
        Logger::Info(L"设置：自定义启动位置 = " + chosen);
    }

    RefreshStartupControls();
    ::InvalidateRect(m_hWnd, nullptr, TRUE);
}

void SettingsDialog::RefreshStartupControls()
{
    if (m_hStartupCombo == nullptr)
    {
        return;
    }

    const core::Settings& settings = core::Settings::Instance();
    const core::StartupLocation location = settings.Startup();
    const std::wstring custom = settings.CustomStartupFolder();

    std::wstring customText = L"自定义";
    if (!custom.empty())
    {
        customText += L" (" + StartupCustomDisplayName(custom) + L")";
    }

    ::SendMessageW(m_hStartupCombo, CB_DELETESTRING, 3, 0);
    ::SendMessageW(m_hStartupCombo, CB_INSERTSTRING, 3,
                   reinterpret_cast<LPARAM>(customText.c_str()));
    ::SendMessageW(m_hStartupCombo, CB_SETDROPPEDWIDTH, Scale(420), 0);

    int sel = 0;
    switch (location)
    {
    case core::StartupLocation::Home:   sel = 1; break;
    case core::StartupLocation::ThisPc: sel = 2; break;
    case core::StartupLocation::Custom: sel = 3; break;
    default:                            sel = 0; break;
    }

    ::SendMessageW(m_hStartupCombo, CB_SETCURSEL, sel, 0);
    ::InvalidateRect(m_hWnd, nullptr, TRUE);
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

bool SettingsDialog::RestoreSystemIntegration()
{
    bool ok = true;

    // 1. 停掉后台宿主进程（Win+E 钩子挂在宿主里，随进程退出一并卸载）
    ui::StopBackgroundHost();

    // 2. 删开机自启注册表项（HKCU，不需要管理员）
    ok = shell::SetAutoStart(false) && ok;

    // 3. 还原文件夹/磁盘的系统默认关联（按注册前的备份原值写回）
    ok = shell::RestoreSystemFileManager() && ok;

    // 4. 一次 UAC 提权：删 System32 代理（认亲后才删）+ 删 Listary 插件
    const bool elevatedOk = shell::ElevatedRunSelf(L"--restore-system", m_hWnd);
    ok = elevatedOk && ok;

    // 5. 三个集成开关置关并落盘；个人偏好（固定项/视图/账本等）不动
    core::Settings& settings = core::Settings::Instance();
    settings.SetBackgroundStartup(false);
    settings.SetTakeOverWinE(false);
    settings.SetShellWindowIntegration(false);
    settings.Save();

    RefreshQuickStart();
    RefreshRegistrationChecks();
    ::InvalidateRect(m_hWnd, nullptr, TRUE);

    return ok;
}

void SettingsDialog::PurgeAllTracesAndExit()
{
    // 先标记：退出时不许再把内存配置写回即将被删掉的配置文件
    core::Settings::Instance().DiscardOnExit();

    // 删除配置目录、自己的注册表键、解压缓存（软件本体不动）
    const bool cleaned = shell::DeleteAllUserTraces();

    // 关掉设置窗口（模态循环随即结束）
    ::DestroyWindow(m_hWnd);

    // 通知后台宿主退出；若本进程就是宿主，StopBackground 会把热窗口的
    // 「关闭即隐藏」标志去掉，下面的 WM_CLOSE 走正常销毁退出
    ui::StopBackgroundHost();

    const std::wstring message = cleaned
        ? L"已清除本软件在电脑上的全部痕迹，重启后为全新状态。\n程序即将退出。"
        : L"部分个人数据清理失败，详情见日志文件。\n程序即将退出。";
    ::MessageBoxW(nullptr, message.c_str(), L"清除完成",
                  cleaned ? MB_ICONINFORMATION | MB_OK : MB_ICONWARNING | MB_OK);

    // 通知主窗口关闭；WM_CLOSE 里的 SaveWindowState 因 DiscardOnExit 不会落盘
    if (m_hOwner != nullptr)
    {
        ::PostMessageW(m_hOwner, WM_CLOSE, 0, 0);
    }
    else
    {
        ::PostQuitMessage(0);
    }
}

void SettingsDialog::OnRestoreButton()
{
    // 第一弹：选择恢复方式（是=仅恢复设置，否=清除所有痕迹，取消=放弃）
    const std::wstring prompt =
        L"请选择恢复方式：\n\n"
        L"【是】仅恢复系统设置\n"
        L"撤销默认文件管理器、System32 代理、Listary 插件、开机自启、Win+E 接管、"
        L"Shell 窗口集成；保留固定项、视图记忆、打开方式账本等个人数据。\n\n"
        L"【否】清除所有痕迹\n"
        L"在上面这些之外，再删除本软件的配置、模板和缓存，只保留软件本体，"
        L"重启后为全新状态。\n\n"
        L"【取消】不做任何改动。";

    const INT_PTR choice = ::MessageBoxW(m_hWnd, prompt.c_str(), L"恢复 / 重置",
                                          MB_YESNOCANCEL | MB_ICONQUESTION);
    if (choice == IDCANCEL)
    {
        return;
    }

    const bool purgeAll = (choice == IDNO);

    // 第二弹：二次确认
    const std::wstring confirm = purgeAll
        ? L"将撤销全部系统集成，并删除配置、模板和缓存等全部个人数据，只保留软件本体。\n"
          L"此操作不可恢复，确定继续吗？"
        : L"将撤销默认管理器、代理、Listary 插件、自启、Win+E 和 Shell 集成，"
          L"个人数据保留。\n确定继续吗？";

    const UINT confirmFlags = MB_OKCANCEL | (purgeAll ? MB_ICONWARNING : MB_ICONQUESTION);
    if (::MessageBoxW(m_hWnd, confirm.c_str(), L"确认", confirmFlags) != IDOK)
    {
        return;
    }

    const bool systemOk = RestoreSystemIntegration();

    if (purgeAll)
    {
        PurgeAllTracesAndExit();
        return;
    }

    ::MessageBoxW(m_hWnd,
                  systemOk ? L"系统设置已恢复。" : L"部分项目恢复失败，详情见日志文件。",
                  L"恢复结果",
                  systemOk ? MB_ICONINFORMATION | MB_OK : MB_ICONWARNING | MB_OK);
}

void SettingsDialog::Draw(HDC dc)
{
    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);

    HBRUSH background = ::CreateSolidBrush(kBackgroundColor);
    ::FillRect(dc, &clientRect, background);
    ::DeleteObject(background);

    auto drawCard = [&](int top, int bottom, const wchar_t* title)
    {
        RECT card = { Scale(12), Scale(top), clientRect.right - Scale(12), Scale(bottom) };

        HPEN borderPen = ::CreatePen(PS_SOLID, 1, RGB(226, 229, 234));
        HGDIOBJ oldPen = ::SelectObject(dc, borderPen);
        HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
        ::RoundRect(dc, card.left, card.top, card.right, card.bottom,
                    Scale(10), Scale(10));
        ::SelectObject(dc, oldPen);
        ::SelectObject(dc, oldBrush);
        ::DeleteObject(borderPen);

        RECT titleRect = { card.left + Scale(16), card.top + Scale(8),
                           card.right - Scale(16), card.top + Scale(30) };
        HGDIOBJ oldFont = ::SelectObject(dc, m_fontSection != nullptr ? m_fontSection : m_font);
        ::SetTextColor(dc, kTitleColor);
        ::SetBkMode(dc, TRANSPARENT);
        ::DrawTextW(dc, title, -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        ::SelectObject(dc, oldFont);
    };

    HGDIOBJ oldFont = ::SelectObject(dc, m_fontTitle);
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, kTitleColor);

    RECT titleRect = { Scale(kPadding), Scale(22),
                       clientRect.right - Scale(kPadding), Scale(54) };
    ::DrawTextW(dc, kTitleText, -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    ::SelectObject(dc, m_font);
    ::SetTextColor(dc, kHintColor);
    RECT subtitleRect = { Scale(kPadding), Scale(56),
                          clientRect.right - Scale(kPadding), Scale(76) };
    ::DrawTextW(dc, kSubtitleText, -1, &subtitleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 分组卡片
    drawCard(82, 242, kViewTitle);
    drawCard(254, 340, kStartupTitle);
    drawCard(352, 480, kQuickTitleShort);
    drawCard(492, 584, kWindowTitleShort);
    drawCard(596, 770, kSectionTitle);

    ::SelectObject(dc, m_font);
    ::SetTextColor(dc, kHintColor);

    RECT viewHint = { Scale(kPadding), Scale(212),
                      clientRect.right - Scale(kPadding), Scale(232) };
    ::DrawTextW(dc, L"隐藏/系统项用浅灰显示；改动立即生效。", -1, &viewHint,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT startupHint = { Scale(kPadding), Scale(318),
                         clientRect.right - Scale(kPadding), Scale(338) };
    ::DrawTextW(dc, L"点“自定义”会弹出选择窗口，支持此电脑、主文件夹。", -1, &startupHint,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT quickHint = { Scale(kPadding), Scale(452),
                       clientRect.right - Scale(kPadding), Scale(472) };
    ::DrawTextW(dc, L"勾选 Win+E 会自动开启后台启动。", -1, &quickHint,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT regHint = { Scale(kPadding), Scale(746),
                     clientRect.right - Scale(kPadding), Scale(766) };
    if (m_lastActionFailed)
    {
        ::SetTextColor(dc, kErrorColor);
        ::DrawTextW(dc, kActionFailedText, -1, &regHint,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    else
    {
        ::SetTextColor(dc, kHintColor);
        ::DrawTextW(dc, L"代理/Listary 需要管理员权限；失败详见日志。", -1, &regHint,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    ::SelectObject(dc, oldFont);
    DrawButton(dc, m_restoreRect, L"恢复/重置", m_hotRestore, m_pressedRestore, true);
    DrawButton(dc, m_closeRect, L"关闭", m_hotClose, m_pressedClose, true);
}

void SettingsDialog::DrawButton(HDC dc, const RECT& rect, const wchar_t* text,
                                bool hot, bool pressed, bool enabled)
{
    ui::FlatButtonVisual visual;
    visual.hot     = hot;
    visual.pressed = pressed;
    visual.enabled = enabled;

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

        if (controlId == kControlIdStartupCombo && notifyCode == CBN_SELENDOK)
        {
            const int sel = static_cast<int>(
                ::SendMessageW(m_hStartupCombo, CB_GETCURSEL, 0, 0));
            if (sel == 3)
            {
                PickCustomStartupFolder();
            }
            else
            {
                ApplyStartupLocation();
            }
            return 0;
        }

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

    case WM_CTLCOLORBTN:
        ::SetBkColor(reinterpret_cast<HDC>(wParam), kBackgroundColor);
        ::SetTextColor(reinterpret_cast<HDC>(wParam), kTitleColor);
        return reinterpret_cast<LRESULT>(::GetStockObject(WHITE_BRUSH));

    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const bool hotClose   = (::PtInRect(&m_closeRect, pt) != FALSE);
        const bool hotRestore = (::PtInRect(&m_restoreRect, pt) != FALSE);

        if (hotClose != m_hotClose)
        {
            m_hotClose = hotClose;
            ::InvalidateRect(hWnd, &m_closeRect, FALSE);
        }
        if (hotRestore != m_hotRestore)
        {
            m_hotRestore = hotRestore;
            ::InvalidateRect(hWnd, &m_restoreRect, FALSE);
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
        if (m_hotRestore || m_pressedRestore)
        {
            m_hotRestore     = false;
            m_pressedRestore = false;
            ::InvalidateRect(hWnd, &m_restoreRect, FALSE);
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
        if (::PtInRect(&m_restoreRect, pt))
        {
            m_pressedRestore = true;
            ::SetCapture(hWnd);
            ::InvalidateRect(hWnd, &m_restoreRect, FALSE);
            return 0;
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
        if (m_pressedRestore)
        {
            m_pressedRestore = false;
            ::ReleaseCapture();
            const bool inside = (::PtInRect(&m_restoreRect, pt) != FALSE);
            ::InvalidateRect(hWnd, &m_restoreRect, FALSE);
            if (inside)
            {
                OnRestoreButton();
            }
            return 0;
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
        m_hStartupCombo = nullptr;
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
