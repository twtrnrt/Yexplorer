// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/OverlaySettingsDialog.cpp
//
// 「同步状态角标」配置窗口的实现。界面说明见 OverlaySettingsDialog.h。
//
// 「同步状态角标」配置窗口的实现。界面说明见 OverlaySettingsDialog.h。
//
// 套列表来自 ShellOverlay 管理器的注册表枚举（同族归并），已存配置的名称 / 范围
// 会合并回去；勾选 / ↑↓ / chip 增删 / 悬停路径在此编辑，确定才写回。
// ============================================================================

#include "ui/OverlaySettingsDialog.h"

#include "core/Logger.h"
#include "shell/ShellOverlay.h"
#include "ui/FlatDraw.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <vsstyle.h>

#include <algorithm>
#include <string>

namespace ui
{

namespace
{

const wchar_t* const kClassName = L"FileExplorerOverlaySettingsDialog";

const int kControlIdList = 2101;

// 窗口客户区尺寸（逻辑像素）
const int kDialogWidth  = 660;
const int kDialogHeight = 460;
const int kPadding      = 14;
const int kHintHeight   = 22;
const int kButtonHeight = 28;

// 列宽（逻辑像素）：显示范围列吃满剩余宽度
const int kPriColumnWidth  = 96;
const int kNameColumnWidth  = 170;

const int kRowHeight = 40;

// 配色（与「打开方式」窗口同一套）
const COLORREF kRowLineColor     = RGB(232, 235, 239);
const COLORREF kRowSelectedColor = RGB(232, 240, 254);
const COLORREF kRowTextColor     = RGB(31, 35, 40);
const COLORREF kHintTextColor    = RGB(96, 103, 111);
const COLORREF kFrameColor       = RGB(232, 235, 239);

const wchar_t* const kHintLabel = L"勾选「全部路径」外的目录不会询问对应处理器；白名单为空即不显示状态。";
const wchar_t* const kPriLabel  = L"优先级";
const wchar_t* const kNameLabel = L"应用名";
const wchar_t* const kScopeLabel = L"显示范围";

// 取路径里的文件夹名（chip 上显示用）；悬停 tooltip 仍给完整路径
std::wstring ShortNameOf(const std::wstring& path)
{
    const wchar_t* name = ::PathFindFileNameW(path.c_str());
    return (name != nullptr && *name != L'\0') ? std::wstring(name) : path;
}

} // namespace

// ============================================================================
// 构造 / 析构
// ============================================================================

OverlaySettingsDialog::OverlaySettingsDialog(HWND owner)
    : m_hOwner(owner)
{
    SetDpiSource(owner);
}

OverlaySettingsDialog::~OverlaySettingsDialog()
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

bool OverlaySettingsDialog::EnsureClassRegistered()
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

bool OverlaySettingsDialog::ShowModal()
{
    // 从注册表枚举归并出真实套列表，并合并已存配置的名称 / 范围；
    // 注册表一台处理器都没有时，退回已存配置（可能为空）。
    m_sets = shell::OverlayManager::Instance().ListSets();
    if (m_sets.empty())
    {
        m_sets = core::Settings::Instance().OverlaySets();
    }
    m_confirmed = false;

    if (!CreateDialogWindow())
    {
        return false;
    }

    RunModalLoop(m_hOwner, m_hList);

    if (m_confirmed)
    {
        SaveNow();
    }

    return m_confirmed;
}

void OverlaySettingsDialog::SaveNow()
{
    core::Settings::Instance().SetOverlaySets(m_sets);
    core::Settings::Instance().Save();
}

// ============================================================================
// 建窗
// ============================================================================

bool OverlaySettingsDialog::CreateDialogWindow()
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
                               L"同步状态角标配置",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               x, y, width, height,
                               m_hOwner,
                               nullptr,
                               ::GetModuleHandleW(nullptr),
                               this);

    if (m_hWnd == nullptr)
    {
        Logger::Error(L"角标配置窗口创建失败");
        return false;
    }

    CreateControls();
    Layout();
    return true;
}

void OverlaySettingsDialog::CreateControls()
{
    m_hList = ::CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER | LVS_NOSORTHEADER | LVS_OWNERDRAWFIXED,
        0, 0, 0, 0,
        m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlIdList)),
        ::GetModuleHandleW(nullptr), nullptr);

    if (m_hList != nullptr)
    {
        ::SendMessageW(m_hList, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        ::SendMessageW(m_hList, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                       LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        column.fmt  = LVCFMT_LEFT;

        column.pszText = const_cast<LPWSTR>(L"优先级");
        column.cx      = Scale(kPriColumnWidth);
        ::SendMessageW(m_hList, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));

        column.pszText = const_cast<LPWSTR>(L"应用名");
        column.cx      = Scale(kNameColumnWidth);
        ::SendMessageW(m_hList, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));

        column.pszText = const_cast<LPWSTR>(L"显示范围");
        column.cx      = Scale(200);
        ::SendMessageW(m_hList, LVM_INSERTCOLUMNW, 2, reinterpret_cast<LPARAM>(&column));

        ReloadList();
    }
}

// 工作副本 → 列表
void OverlaySettingsDialog::ReloadList()
{
    if (m_hList == nullptr)
    {
        return;
    }

    ListView_DeleteAllItems(m_hList);
    m_hits.assign(m_sets.size(), RowHit{});

    for (int i = 0; i < static_cast<int>(m_sets.size()); ++i)
    {
        LVITEMW item = {};
        item.mask    = LVIF_TEXT;
        item.iItem   = i;
        item.pszText = const_cast<LPWSTR>(m_sets[i].name.c_str());
        ListView_InsertItem(m_hList, &item);
    }
}

// ============================================================================
// 布局
// ============================================================================

void OverlaySettingsDialog::Layout()
{
    if (m_hWnd == nullptr || m_hList == nullptr)
    {
        return;
    }

    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);

    const int padding = Scale(kPadding);
    const int clientWidth  = clientRect.right;
    const int clientHeight = clientRect.bottom;

    const int frameTop    = padding + Scale(kHintHeight) + Scale(8);
    const int frameBottom = clientHeight - padding - Scale(kButtonHeight) - Scale(8);

    m_listRect.left   = padding;
    m_listRect.top    = frameTop;
    m_listRect.right  = clientWidth - padding;
    m_listRect.bottom = frameBottom;

    ::MoveWindow(m_hList,
                 m_listRect.left, m_listRect.top,
                 m_listRect.right - m_listRect.left,
                 m_listRect.bottom - m_listRect.top,
                 TRUE);

    // 显示范围列吃满剩余宽度
    const int listWidth = m_listRect.right - m_listRect.left;
    const int scopeWidth = listWidth - Scale(kPriColumnWidth) - Scale(kNameColumnWidth);
    ::SendMessageW(m_hList, LVM_SETCOLUMNWIDTH, 2, std::max(Scale(180), scopeWidth));

    // 底部按钮：确定（右，主按钮）/ 取消（其左）
    const int widthOk     = Scale(88);
    const int widthCancel = Scale(76);
    const int gap         = Scale(8);
    const int buttonY     = clientHeight - padding - Scale(kButtonHeight);
    const int buttonHeight = Scale(kButtonHeight);

    int rightEdge = clientWidth - padding;
    m_buttonRects[ButtonOk].right  = rightEdge;
    m_buttonRects[ButtonOk].left    = rightEdge - widthOk;
    rightEdge = m_buttonRects[ButtonOk].left - gap;

    m_buttonRects[ButtonCancel].right = rightEdge;
    m_buttonRects[ButtonCancel].left  = rightEdge - widthCancel;

    for (int index = 0; index < ButtonCount; ++index)
    {
        m_buttonRects[index].top    = buttonY;
        m_buttonRects[index].bottom = buttonY + buttonHeight;
    }
}

// ============================================================================
// 绘制
// ============================================================================

void OverlaySettingsDialog::Draw(HDC dc)
{
    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);

    HBRUSH background = ::CreateSolidBrush(RGB(248, 248, 248));
    ::FillRect(dc, &clientRect, background);
    ::DeleteObject(background);

    if (m_font != nullptr)
    {
        ::SelectObject(dc, m_font);
    }
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, kHintTextColor);

    // 列名（列表上方，按各列对齐）
    const int priW  = static_cast<int>(::SendMessageW(m_hList, LVM_GETCOLUMNWIDTH, 0, 0));
    const int nameW = static_cast<int>(::SendMessageW(m_hList, LVM_GETCOLUMNWIDTH, 1, 0));

    RECT priLabel  = { m_listRect.left + Scale(8), Scale(kPadding),
                       m_listRect.left + priW, Scale(kPadding) + Scale(kHintHeight) };
    RECT nameLabel = { m_listRect.left + priW + Scale(8), Scale(kPadding),
                       m_listRect.left + priW + nameW, Scale(kPadding) + Scale(kHintHeight) };
    RECT scopeLabel = { m_listRect.left + priW + nameW + Scale(8), Scale(kPadding),
                        m_listRect.right - Scale(8), Scale(kPadding) + Scale(kHintHeight) };
    ::DrawTextW(dc, kPriLabel,  -1, &priLabel,  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ::DrawTextW(dc, kNameLabel, -1, &nameLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ::DrawTextW(dc, kScopeLabel, -1, &scopeLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 列表外那圈框
    HPEN pen = ::CreatePen(PS_SOLID, 1, kFrameColor);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(dc, m_listRect.left, m_listRect.top, m_listRect.right, m_listRect.bottom);
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);

    // 底部提示词：放在取消按钮左侧、靠右对齐
    {
        RECT hintRect = { Scale(kPadding), m_buttonRects[ButtonCancel].top,
                          m_buttonRects[ButtonCancel].left - Scale(8),
                          m_buttonRects[ButtonCancel].bottom };
        ::SetTextColor(dc, kHintTextColor);
        ::DrawTextW(dc, kHintLabel, -1, &hintRect,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // 底部按钮
    DrawFlatButton(dc, m_buttonRects[ButtonCancel], L"取消", m_font, {});
    ui::FlatButtonVisual okVisual;
    okVisual.primary = true;
    DrawFlatButton(dc, m_buttonRects[ButtonOk], L"确定", m_font, okVisual);
}

void OverlaySettingsDialog::DrawCheckBox(HDC dc, const RECT& box, bool checked)
{
    RECT themed = box;
    ::InflateRect(&themed, -Scale(1), -Scale(1));

    HTHEME theme = ::OpenThemeData(m_hWnd, L"Button");
    if (theme != nullptr)
    {
        const int state = checked ? CBS_CHECKEDNORMAL : CBS_UNCHECKEDNORMAL;
        ::DrawThemeBackground(theme, dc, BP_CHECKBOX, state, &themed, nullptr);
        ::CloseThemeData(theme);
        return;
    }

    HPEN pen = ::CreatePen(PS_SOLID, 1, RGB(120, 120, 120));
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(WHITE_BRUSH));
    ::Rectangle(dc, box.left, box.top, box.right, box.bottom);
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);

    if (checked)
    {
        const int width  = box.right - box.left;
        const int height = box.bottom - box.top;
        HPEN markPen = ::CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
        oldPen = ::SelectObject(dc, markPen);
        ::MoveToEx(dc, box.left + width / 4, box.top + height / 2, nullptr);
        ::LineTo(dc, box.left + width * 2 / 5, box.top + height * 3 / 4);
        ::LineTo(dc, box.right - width / 5, box.top + height / 4);
        ::SelectObject(dc, oldPen);
        ::DeleteObject(markPen);
    }
}

// 画一个小方形箭头按钮（↑ / ↓ / + / × 都走这个底）
static void DrawGlyphButton(HDC dc, const RECT& rect, const wchar_t* glyph,
                            bool hot, bool enabled)
{
    ui::FlatButtonVisual visual;
    visual.hot     = hot;
    visual.enabled = enabled;
    ui::DrawBorderedRect(dc, rect,
                         enabled ? ui::kButtonFace : ui::kButtonDisabledFace,
                         enabled ? ui::kButtonBorder : ui::kButtonDisabledBorder);

    if (enabled)
    {
        ::SetTextColor(dc, ui::kButtonText);
    }
    else
    {
        ::SetTextColor(dc, ui::kButtonDisabledText);
    }
    ::SetBkMode(dc, TRANSPARENT);
    ::DrawTextW(dc, glyph, -1, const_cast<LPRECT>(&rect),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void OverlaySettingsDialog::DrawRow(HDC dc, const RECT& rowRect, int index, bool selected)
{
    if (index < 0 || index >= static_cast<int>(m_sets.size()))
    {
        return;
    }

    const core::OverlaySet& set = m_sets[index];
    RowHit& hit = m_hits[index];

    // 底色
    HBRUSH background = ::CreateSolidBrush(selected ? kRowSelectedColor : RGB(255, 255, 255));
    ::FillRect(dc, &rowRect, background);
    ::DeleteObject(background);

    const int rowHeight = rowRect.bottom - rowRect.top;
    const int centerY   = rowRect.top + rowHeight / 2;

    const int priWidth  = static_cast<int>(::SendMessageW(m_hList, LVM_GETCOLUMNWIDTH, 0, 0));
    const int nameWidth = static_cast<int>(::SendMessageW(m_hList, LVM_GETCOLUMNWIDTH, 1, 0));
    const int scopeLeft = priWidth + nameWidth;

    // ---- 优先级列：序号 + ↑ / ↓ ----
    // 箭头按钮窄一半（宽 11），高度不变；两箭头中间间隔 2px
    const int btnW    = Scale(11);
    const int btnH    = Scale(22);
    const int btnGap  = Scale(2);

    RECT upRect = { Scale(34), centerY - btnH / 2,
                    Scale(34) + btnW, centerY - btnH / 2 + btnH };
    RECT downRect = { upRect.right + btnGap, centerY - btnH / 2,
                      upRect.right + btnGap + btnW, centerY - btnH / 2 + btnH };
    hit.up   = upRect;
    hit.down = downRect;

    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, kRowTextColor);
    RECT noRect = { Scale(8), rowRect.top, Scale(30), rowRect.bottom };
    ::DrawTextW(dc, std::to_wstring(index + 1).c_str(), -1, &noRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DrawGlyphButton(dc, upRect,   L"↑", false, index > 0);
    DrawGlyphButton(dc, downRect, L"↓", false, index + 1 < static_cast<int>(m_sets.size()));

    // ---- 应用名列 ----
    RECT nameRect = { priWidth + Scale(8), rowRect.top,
                       scopeLeft - Scale(4), rowRect.bottom - 1 };
    ::SetTextColor(dc, kRowTextColor);
    ::DrawTextW(dc, set.name.c_str(), -1, &nameRect,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    // ---- 显示范围列 ----
    const int checkSize = Scale(14);
    RECT checkRect = { scopeLeft + Scale(8), centerY - checkSize / 2,
                       scopeLeft + Scale(8) + checkSize, centerY - checkSize / 2 + checkSize };
    hit.check = checkRect;
    DrawCheckBox(dc, checkRect, set.allPaths);

    int cursorX = checkRect.right + Scale(8);
    const int scopeRight = m_listRect.right - m_listRect.left - Scale(8);

    if (set.allPaths)
    {
        RECT allRect = { cursorX, rowRect.top, scopeRight, rowRect.bottom };
        ::SetTextColor(dc, kHintTextColor);
        ::DrawTextW(dc, L"处理全部路径", -1, &allRect,
                    DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        hit.chips.clear();
        hit.add = RECT{};
        return;
    }

    // 白名单：先排 chip，末尾留「+」按钮位置
    const int chipHeight = Scale(24);
    const int chipGap    = Scale(6);
    const int addSize    = Scale(24);
    const int addLeft    = scopeRight - addSize;

    hit.chips.clear();

    // 末尾「+」按钮位置（无论白名单是否为空都要可点，方便从空列表加第一条）
    hit.add = { addLeft, centerY - addSize / 2, addLeft + addSize, centerY - addSize / 2 + addSize };

    // 不勾全部路径、又没选任何文件夹 = 这套禁用，提示「不显示状态」；右侧仍保留「+」
    if (set.paths.empty())
    {
        RECT emptyRect = { cursorX, rowRect.top, addLeft - chipGap, rowRect.bottom };
        ::SetTextColor(dc, kHintTextColor);
        ::DrawTextW(dc, L"不显示状态", -1, &emptyRect,
                    DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        DrawGlyphButton(dc, hit.add, L"+", false, true);
        return;
    }

    HDC measureDc = ::GetDC(m_hWnd);
    HGDIOBJ oldMeasureFont = nullptr;
    if (measureDc != nullptr && m_font != nullptr)
    {
        oldMeasureFont = ::SelectObject(measureDc, m_font);
    }

    for (int i = 0; i < static_cast<int>(set.paths.size()); ++i)
    {
        const std::wstring shortName = ShortNameOf(set.paths[i]);

        SIZE textSize = {};
        ::GetTextExtentPoint32W(measureDc, shortName.c_str(),
                                static_cast<int>(shortName.size()), &textSize);

        // 理想宽 = 文字 + 左右留白 + × 区；最小宽 = 只留 ×
        const int idealWidth = textSize.cx + Scale(18) + Scale(14);
        const int minWidth   = Scale(24);
        const int available  = addLeft - chipGap - cursorX;
        int chipWidth = idealWidth;
        if (chipWidth > available)
        {
            chipWidth = std::max(minWidth, std::min(idealWidth, available));
        }

        RECT chip = { cursorX, centerY - chipHeight / 2,
                      cursorX + chipWidth, centerY - chipHeight / 2 + chipHeight };

        // chip 底
        ui::DrawBorderedRect(dc, chip, RGB(243, 245, 248), RGB(206, 212, 220));

        // × 区（右侧固定 14px）
        RECT xRect = { chip.right - Scale(14), chip.top, chip.right, chip.bottom };
        ::SetTextColor(dc, kHintTextColor);
        ::DrawTextW(dc, L"×", -1, &xRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // 文件名（宽够才画，过窄只留 ×）
        if (chipWidth > minWidth)
        {
            RECT textRect = { chip.left + Scale(8), chip.top,
                              xRect.left - Scale(2), chip.bottom };
            ::SetTextColor(dc, kRowTextColor);
            ::DrawTextW(dc, shortName.c_str(), -1, &textRect,
                        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }

        hit.chips.push_back(chip);
        cursorX = chip.right + chipGap;
    }

    if (measureDc != nullptr)
    {
        if (oldMeasureFont != nullptr)
        {
            ::SelectObject(measureDc, oldMeasureFont);
        }
        ::ReleaseDC(m_hWnd, measureDc);
    }

    // 末尾「+」
    DrawGlyphButton(dc, hit.add, L"+", false, true);
}

// ============================================================================
// 动作
// ============================================================================

void OverlaySettingsDialog::MoveSet(int index, int delta)
{
    const int target = index + delta;
    if (index < 0 || index >= static_cast<int>(m_sets.size()) ||
        target < 0 || target >= static_cast<int>(m_sets.size()))
    {
        return;
    }

    std::swap(m_sets[index], m_sets[target]);
    ReloadList();
    ::UpdateWindow(m_hList);
}

void OverlaySettingsDialog::ToggleScope(int index)
{
    if (index < 0 || index >= static_cast<int>(m_sets.size()))
    {
        return;
    }

    m_sets[index].allPaths = !m_sets[index].allPaths;
    ListView_RedrawItems(m_hList, index, index);
    ::UpdateWindow(m_hList);
}

void OverlaySettingsDialog::AddPath(int index)
{
    if (index < 0 || index >= static_cast<int>(m_sets.size()))
    {
        return;
    }

    IFileOpenDialog* dialog = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&dialog))) || dialog == nullptr)
    {
        return;
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options)))
    {
        dialog->SetOptions((options | FOS_PICKFOLDERS) & ~FOS_FORCEFILESYSTEM);
    }
    dialog->SetTitle(L"选择要纳入同步角标的文件夹");

    if (FAILED(dialog->Show(m_hWnd)))
    {
        dialog->Release();
        return;
    }

    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr)
    {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr)
        {
            std::wstring newPath = path;
            ::CoTaskMemFree(path);

            // 去重（忽略大小写）
            bool exists = false;
            for (const std::wstring& p : m_sets[index].paths)
            {
                if (_wcsicmp(p.c_str(), newPath.c_str()) == 0)
                {
                    exists = true;
                    break;
                }
            }
            if (!exists)
            {
                m_sets[index].paths.push_back(newPath);
            }
        }
        item->Release();
    }

    dialog->Release();
    ReloadList();
    ::UpdateWindow(m_hList);
}

void OverlaySettingsDialog::RemovePath(int index, int chip)
{
    if (index < 0 || index >= static_cast<int>(m_sets.size()))
    {
        return;
    }
    if (chip < 0 || chip >= static_cast<int>(m_sets[index].paths.size()))
    {
        return;
    }

    m_sets[index].paths.erase(m_sets[index].paths.begin() + chip);
    ReloadList();
    ::UpdateWindow(m_hList);
}

// ============================================================================
// 消息
// ============================================================================

LRESULT OverlaySettingsDialog::HandleMessage(HWND, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SIZE:
        Layout();
        return 0;

    case WM_MEASUREITEM:
    {
        MEASUREITEMSTRUCT* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (measure != nullptr && measure->CtlID == static_cast<UINT>(kControlIdList))
        {
            measure->itemHeight = static_cast<UINT>(Scale(kRowHeight));
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (draw != nullptr && draw->CtlID == static_cast<UINT>(kControlIdList) &&
            draw->itemID != static_cast<UINT>(-1))
        {
            DrawRow(draw->hDC, draw->rcItem, static_cast<int>(draw->itemID),
                    (draw->itemState & ODS_SELECTED) != 0);
            return TRUE;
        }
        break;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT paint = {};
        HDC dc = ::BeginPaint(m_hWnd, &paint);
        Draw(dc);
        ::EndPaint(m_hWnd, &paint);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_NOTIFY:
    {
        const NMHDR* header = reinterpret_cast<const NMHDR*>(lParam);
        if (header->hwndFrom != m_hList)
        {
            break;
        }

        if (header->code == NM_CLICK)
        {
            const NMITEMACTIVATE* activate = reinterpret_cast<const NMITEMACTIVATE*>(lParam);
            const int index = activate->iItem;
            const POINT pt  = activate->ptAction;

            if (index < 0 || index >= static_cast<int>(m_hits.size()))
            {
                return 0;
            }

            const RowHit& hit = m_hits[index];

            if (::PtInRect(&hit.up, pt))
            {
                MoveSet(index, -1);
            }
            else if (::PtInRect(&hit.down, pt))
            {
                MoveSet(index, 1);
            }
            else if (::PtInRect(&hit.check, pt))
            {
                ToggleScope(index);
            }
            else if (::PtInRect(&hit.add, pt))
            {
                AddPath(index);
            }
            else
            {
                for (int i = 0; i < static_cast<int>(hit.chips.size()); ++i)
                {
                    if (::PtInRect(&hit.chips[i], pt))
                    {
                        RemovePath(index, i);
                        break;
                    }
                }
            }
        }
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);

        int hot = -1;
        for (int index = 0; index < ButtonCount; ++index)
        {
            if (m_buttonRects[index].right > m_buttonRects[index].left &&
                ::PtInRect(&m_buttonRects[index], POINT{x, y}))
            {
                hot = index;
                break;
            }
        }
        if (hot != m_hotButton)
        {
            m_hotButton = hot;
            ::InvalidateRect(m_hWnd, nullptr, FALSE);
        }

        TRACKMOUSEEVENT track = {};
        track.cbSize    = sizeof(track);
        track.dwFlags   = TME_LEAVE;
        track.hwndTrack = m_hWnd;
        ::TrackMouseEvent(&track);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (m_hotButton != -1)
        {
            m_hotButton = -1;
            ::InvalidateRect(m_hWnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN:
    {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        for (int index = 0; index < ButtonCount; ++index)
        {
            if (m_buttonRects[index].right > m_buttonRects[index].left &&
                ::PtInRect(&m_buttonRects[index], POINT{x, y}))
            {
                m_pressedButton = index;
                ::SetCapture(m_hWnd);
                ::InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;
            }
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        if (::GetCapture() == m_hWnd)
        {
            ::ReleaseCapture();
        }

        const int pressed = m_pressedButton;
        m_pressedButton = -1;
        ::InvalidateRect(m_hWnd, nullptr, FALSE);

        if (pressed < 0)
        {
            return 0;
        }

        if (::PtInRect(&m_buttonRects[pressed], POINT{x, y}))
        {
            if (pressed == ButtonOk)
            {
                m_confirmed = true;
            }
            ::DestroyWindow(m_hWnd);
        }
        return 0;
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
