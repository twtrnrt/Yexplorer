// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/SettingsDialog.cpp
//
// 「设置」窗口的实现。说明见 SettingsDialog.h。
//
// 【为什么白底】
//   里面唯一的系统控件是复选框，系统复选框会自己刷一块白底；
//   窗口底色也用白色，两者就融在一起了，不会出现"白方块"。
// ============================================================================

#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <commctrl.h>

#include "ui/SettingsDialog.h"

#include "core/Logger.h"
#include "core/Settings.h"
#include "shell/DefaultManager.h"
#include "shell/ExplorerProxy.h"      // System32 里 explorer.exe 代理的部署/移除
#include "shell/ListaryIntegration.h" // Listary 插件
#include "ui/FlatDraw.h"

namespace
{

const wchar_t* kClassName = L"FileExplorerSettingsDialog";

// 客户区大小（不含标题栏与边框）
const int kClientWidth  = 420;
const int kClientHeight = 540;
const int kPadding      = 24;

// 控件 ID
const int kControlIdMenuIcons     = 4001;
const int kControlIdClose         = 4002;
const int kControlIdShowHidden    = 4003;   // 显示隐藏的文件和文件夹
const int kControlIdShowSystem    = 4004;   // 显示受保护的操作系统文件
const int kControlIdShellWindow   = 4005;   // 允许其它程序定位到我打开的文件
const int kControlIdDefaultManager= 4006;   // 注册为默认文件管理器
const int kControlIdProxy         = 4007;   // 部署 System32 代理
const int kControlIdListary       = 4008;   // 安装 Listary 插件

// 颜色
const COLORREF kBackgroundColor = RGB(255, 255, 255);
const COLORREF kTitleColor      = RGB(32, 32, 32);
const COLORREF kHintColor       = RGB(96, 103, 111);
const COLORREF kErrorColor      = RGB(178, 34, 34);
const COLORREF kDividerColor    = RGB(232, 235, 239);

const wchar_t* kTitleText = L"设置";

const wchar_t* kHintText1 = L"勾选后，「打开方式」和「新建」菜单里的每一项前面会显示小图标。";
const wchar_t* kHintText2 = L"不勾选就是纯文字菜单（默认，弹出更快）。改动立即保存。";

// 查看选项
const wchar_t* kHiddenCheckText = L"显示隐藏的文件和文件夹";
const wchar_t* kSystemCheckText = L"显示受保护的操作系统文件";
const wchar_t* kHiddenHintText  = L"隐藏项用浅灰文字标示。改动立即生效，不用重启程序。";

// 注册与集成：4 个独立开关
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

    // ---- 模态循环（禁用 owner → 显示 → 消息循环 → 恢复）----
    RunModalLoop(m_hOwner);

    return true;
}

bool SettingsDialog::CreateDialogWindow()
{
    if (!EnsureClassRegistered())
    {
        return false;
    }

    // ---- 字体：正文用系统消息字体；标题同款加粗加大一号 ----
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

    // ---- 按客户区大小反推窗口大小（要把标题栏和边框算进去）----
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

    // 居中于主窗口
    RECT ownerRect = {};
    if (m_hOwner != nullptr)
    {
        ::GetWindowRect(m_hOwner, &ownerRect);
    }

    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top  + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    m_hWnd = ::CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                               kClassName,
                               L"设置",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               x, y, width, height,
                               m_hOwner,
                               nullptr,
                               ::GetModuleHandleW(nullptr),
                               this);

    if (m_hWnd == nullptr)
    {
        Logger::Error(L"设置窗口创建失败");
        return false;
    }

    auto createCheck = [this](const wchar_t* text, int id) -> HWND
    {
        HWND check = ::CreateWindowExW(0, L"BUTTON", text,
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
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
    m_hCheck       = createCheck(L"右键菜单显示图标", kControlIdMenuIcons);
    m_hCheckHidden = createCheck(kHiddenCheckText, kControlIdShowHidden);
    m_hCheckSystem = createCheck(kSystemCheckText, kControlIdShowSystem);

    if (m_hCheck != nullptr)
    {
        SetChecked(m_hCheck, core::Settings::Instance().MenuIcons());
    }
    if (m_hCheckHidden != nullptr)
    {
        SetChecked(m_hCheckHidden, core::Settings::Instance().ShowHiddenFiles());
    }
    if (m_hCheckSystem != nullptr)
    {
        SetChecked(m_hCheckSystem, core::Settings::Instance().ShowSystemFiles());
    }

    // ---- 注册与集成：4 个独立开关 ----
    m_hCheckDefaultManager = createCheck(kDefaultCheckText, kControlIdDefaultManager);
    m_hCheckProxy          = createCheck(kProxyCheckText, kControlIdProxy);
    m_hCheckListary        = createCheck(kListaryCheckText, kControlIdListary);
    m_hCheckShellWindow    = createCheck(kShellWindowCheckText, kControlIdShellWindow);

    // 按实际状态勾选；Listary 没装就禁用
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
    place(m_hCheckHidden, 132);
    place(m_hCheckSystem, 160);

    // 注册与集成
    place(m_hCheckDefaultManager, 262);
    place(m_hCheckProxy,          310);
    place(m_hCheckListary,        358);
    place(m_hCheckShellWindow,    406);

    // 关闭按钮：右下角
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

    // 一改就写盘，不做"确定/取消"
    settings.Save();

    Logger::Info(std::wstring(L"设置：右键菜单显示图标 = ") + (showIcons ? L"开" : L"关") +
                 L"，显示隐藏文件 = " + (showHidden ? L"开" : L"关") +
                 L"，显示系统文件 = " + (showSystem ? L"开" : L"关") +
                 L"，Shell 窗口集成 = " + (shellWindow ? L"开" : L"关"));
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

    // 以实际状态为准：UAC 取消、写注册表失败、Listary 没装，都回退勾选
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

    // ---- 背景 ----
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

    // ---- 说明两行（右键菜单图标那个开关）----
    ::SelectObject(dc, m_font);
    ::SetTextColor(dc, kHintColor);

    RECT hintRect = { Scale(kPadding), Scale(kPadding) + Scale(80),
                      clientRect.right - Scale(kPadding), Scale(kPadding) + Scale(126) };

    ::DrawTextW(dc, kHintText1, -1, &hintRect, DT_LEFT | DT_TOP | DT_WORDBREAK);

    hintRect.top += Scale(20);
    ::DrawTextW(dc, kHintText2, -1, &hintRect, DT_LEFT | DT_TOP | DT_WORDBREAK);

    // ---- 显示隐藏 / 系统文件那一段的说明 ----
    RECT hiddenHintRect = { Scale(kPadding), Scale(kPadding) + Scale(188),
                            clientRect.right - Scale(kPadding), Scale(kPadding) + Scale(214) };

    ::DrawTextW(dc, kHiddenHintText, -1, &hiddenHintRect, DT_LEFT | DT_TOP | DT_SINGLELINE);

    // ---- 分隔线 ----
    const int dividerY = Scale(kPadding) + Scale(216);

    HPEN dividerPen = ::CreatePen(PS_SOLID, 1, kDividerColor);
    HGDIOBJ oldPen  = ::SelectObject(dc, dividerPen);

    ::MoveToEx(dc, Scale(kPadding), dividerY, nullptr);
    ::LineTo(dc, clientRect.right - Scale(kPadding), dividerY);

    ::SelectObject(dc, oldPen);
    ::DeleteObject(dividerPen);

    // ---- 「注册与集成」小标题 ----
    RECT sectionRect = { Scale(kPadding), Scale(kPadding) + Scale(228),
                         clientRect.right - Scale(kPadding), Scale(kPadding) + Scale(252) };

    ::SetTextColor(dc, kTitleColor);
    ::DrawTextW(dc, kSectionTitle, -1, &sectionRect, DT_LEFT | DT_TOP | DT_SINGLELINE);

    // ---- 每个复选框下面的说明 ----
    ::SetTextColor(dc, kHintColor);

    auto drawHint = [&](int y, const wchar_t* text)
    {
        RECT rect = { Scale(kPadding), Scale(kPadding) + Scale(y),
                      clientRect.right - Scale(kPadding), Scale(kPadding) + Scale(y + 20) };
        ::DrawTextW(dc, text, -1, &rect, DT_LEFT | DT_TOP | DT_SINGLELINE);
    };

    drawHint(284, kDefaultHintText);
    drawHint(332, kProxyHintText);
    drawHint(380, shell::IsListaryInstalled() ? kListaryHintText : kNoListaryHintText);
    drawHint(428, kShellWindowHintText);

    // ---- 上一次操作失败 ----
    if (m_lastActionFailed)
    {
        RECT errorRect = { Scale(kPadding), Scale(kPadding) + Scale(456),
                           clientRect.right - Scale(kPadding), Scale(kPadding) + Scale(478) };
        ::SetTextColor(dc, kErrorColor);
        ::DrawTextW(dc, kActionFailedText, -1, &errorRect, DT_LEFT | DT_TOP | DT_SINGLELINE);
    }

    ::SelectObject(dc, oldFont);

    // ---- 关闭按钮 ----
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

        // 查看类：菜单图标 / 隐藏 / 系统 / Shell 窗口
        if (controlId == kControlIdMenuIcons ||
            controlId == kControlIdShowHidden ||
            controlId == kControlIdShowSystem ||
            controlId == kControlIdShellWindow)
        {
            ApplyCheckState();
            return 0;
        }

        // 注册类：4 个独立动作
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

        const bool hotClose = (::PtInRect(&m_closeRect, pt) != FALSE);
        if (hotClose != m_hotClose)
        {
            m_hotClose = hotClose;
            ::InvalidateRect(hWnd, &m_closeRect, FALSE);
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
        return 0;

    case WM_LBUTTONDOWN:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        if (::PtInRect(&m_closeRect, pt))
        {
            m_pressedClose = true;
            ::SetCapture(hWnd);
            ::InvalidateRect(hWnd, &m_closeRect, FALSE);
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
