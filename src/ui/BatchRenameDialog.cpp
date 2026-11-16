// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/BatchRenameDialog.cpp
//
// 「批量重命名」窗口的实现。说明见 BatchRenameDialog.h。
//
// 【这一版的边界】
//   界面 + 实时预览是完整的：改任何规则都会立刻重算右边列表。
//   点「应用」目前只把结果回传给调用方（真正落盘的改名、撤销记录、
//   进度条留到下一步接），所以不会动文件。
//
// 【为什么用原生控件】
//   编辑框、下拉框、复选框、列表都是系统的：输入法、Tab 焦点、
//   键盘操作全部免费拿到，和系统对话框手感一致；只有窗口背景、
//   标题、按钮自己画（和「设置」「打开方式」两个窗口一套风格）。
// ============================================================================

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

#include <algorithm>
#include <string.h>
#include <cwctype>

#include "ui/BatchRenameDialog.h"

#include "core/Logger.h"

namespace
{

const wchar_t* kClassName = L"FileExplorerBatchRename";

// 客户区大小（96 DPI 下的逻辑尺寸）
const int kClientWidth  = 940;
const int kClientHeight = 624;
const int kPadding      = 20;

// 预览列表的位置
const int kListTop    = 78;
const int kListHeight = 268;

// 规则区：两栏，每栏 5 行
const int kRuleTop      = 364;
const int kRuleRowHeight = 34;
const int kLeftColumnX  = 20;
const int kLeftColumnW  = 430;
const int kRightColumnX = 490;
const int kRightColumnW = 430;

// 控件 ID
enum : int
{
    kIdPrefix        = 5001,   // 前缀
    kIdSuffix        = 5002,   // 后缀
    kIdFind          = 5003,   // 查找
    kIdReplace       = 5004,   // 替换为
    kIdCaseSensitive = 5005,   // 区分大小写

    kIdNumberEnable  = 5006,   // 加序号
    kIdNumberStart   = 5007,   // 起始
    kIdNumberStep    = 5008,   // 步长
    kIdNumberDigits  = 5009,   // 位数
    kIdNumberPos     = 5010,   // 位置（前缀 / 后缀）
    kIdNumberSep     = 5011,   // 分隔符

    kIdExtMode       = 5012,   // 扩展名：保留 / 改成 / 删除
    kIdExtValue      = 5013,   // 扩展名改成什么
    kIdCaseMode      = 5014,   // 大小写
    kIdSortMode      = 5015,   // 排序依据
    kIdSortReverse   = 5016,   // 反向

    kIdList          = 5100,   // 预览列表
    kIdCancel        = 5101,   // 取消（自绘按钮）
    kIdApply         = 5102    // 应用（自绘按钮）
};

// 颜色（和「设置」窗口一套）
const COLORREF kBackground      = RGB(255, 255, 255);
const COLORREF kTitleColor      = RGB(32, 32, 32);
const COLORREF kHintColor       = RGB(96, 103, 111);
const COLORREF kDividerColor    = RGB(232, 235, 239);
const COLORREF kErrorColor      = RGB(178, 34, 34);
const COLORREF kOkColor         = RGB(0, 120, 60);

const COLORREF kButtonFace       = RGB(252, 252, 252);
const COLORREF kButtonBorder     = RGB(173, 173, 173);
const COLORREF kButtonHotFace    = RGB(240, 242, 245);
const COLORREF kButtonHotBorder  = RGB(120, 150, 200);
const COLORREF kButtonDownFace   = RGB(212, 214, 219);
const COLORREF kButtonDownBorder = RGB(140, 140, 140);

const COLORREF kPrimaryFace       = RGB(0, 103, 192);
const COLORREF kPrimaryHotFace    = RGB(16, 120, 215);
const COLORREF kPrimaryDownFace   = RGB(0, 84, 160);
const COLORREF kPrimaryBorder     = RGB(0, 90, 170);

const wchar_t* kTitleText = L"批量重命名";

// 文件名里不允许出现的字符（Windows 规则）
const wchar_t* kInvalidChars = L"<>:\"/\\|?*";

// ----------------------------------------------------------------------------
// 把一个文件名拆成「主名 + 扩展名（含点）」
// .gitignore 这种"点在最前面"的算没有扩展名
// ----------------------------------------------------------------------------
void SplitFileName(const std::wstring& name, std::wstring& stem, std::wstring& ext)
{
    const size_t dot = name.find_last_of(L'.');

    if (dot == std::wstring::npos || dot == 0)
    {
        stem = name;
        ext.clear();
        return;
    }

    stem = name.substr(0, dot);
    ext  = name.substr(dot);
}

// ----------------------------------------------------------------------------
// 全量替换（区分大小写 / 不区分大小写两种）
// 不区分大小写时只用于"找位置"，替换进去的还是用户写的原文
// ----------------------------------------------------------------------------
std::wstring ReplaceAll(const std::wstring& text, const std::wstring& find,
                       const std::wstring& replacement, bool caseSensitive)
{
    if (find.empty())
    {
        return text;
    }

    std::wstring result;
    result.reserve(text.size());

    const size_t findLength = find.size();
    size_t       i          = 0;

    while (i < text.size())
    {
        bool matched = false;

        if (i + findLength <= text.size())
        {
            if (caseSensitive)
            {
                matched = (::wcsncmp(text.c_str() + i, find.c_str(), findLength) == 0);
            }
            else
            {
                matched = (::_wcsnicmp(text.c_str() + i, find.c_str(), findLength) == 0);
            }
        }

        if (matched)
        {
            result += replacement;
            i += findLength;
        }
        else
        {
            result += text[i];
            ++i;
        }
    }

    return result;
}

// ----------------------------------------------------------------------------
// 转小写（用于重名比较）
// ----------------------------------------------------------------------------
std::wstring ToLowerText(std::wstring text)
{
    ::CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

// ----------------------------------------------------------------------------
// 整数按位数补前导零
// ----------------------------------------------------------------------------
std::wstring FormatNumber(long long value, int digits)
{
    std::wstring text = std::to_wstring(value);

    if (digits > static_cast<int>(text.size()))
    {
        text.insert(0, static_cast<size_t>(digits - static_cast<int>(text.size())), L'0');
    }

    return text;
}

// ----------------------------------------------------------------------------
// 从编辑框读一个小整数（读不出来就用默认值）
// ----------------------------------------------------------------------------
long long ReadInteger(HWND edit, long long fallback, long long minimum)
{
    if (edit == nullptr)
    {
        return fallback;
    }

    wchar_t buffer[64] = {};
    ::GetWindowTextW(edit, buffer, static_cast<int>(_countof(buffer)));

    wchar_t* end = nullptr;
    const long long value = ::wcstoll(buffer, &end, 10);

    if (end == buffer)
    {
        return fallback;
    }

    return (value < minimum) ? minimum : value;
}

} // 匿名命名空间

// ============================================================================
// 生命周期
// ============================================================================

BatchRenameDialog::~BatchRenameDialog()
{
    if (m_font != nullptr)
    {
        ::DeleteObject(m_font);
    }
    if (m_fontTitle != nullptr)
    {
        ::DeleteObject(m_fontTitle);
    }
    if (m_fontSmall != nullptr)
    {
        ::DeleteObject(m_fontSmall);
    }
    if (m_whiteBrush != nullptr)
    {
        ::DeleteObject(m_whiteBrush);
    }
}

int BatchRenameDialog::Scale(int value) const
{
    int dpi = (m_hWnd != nullptr) ? ::GetDpiForWindow(m_hWnd) : ::GetDpiForWindow(m_hOwner);
    if (dpi <= 0)
    {
        dpi = 96;
    }
    return ::MulDiv(value, dpi, 96);
}

bool BatchRenameDialog::EnsureClassRegistered()
{
    static bool registered = false;
    if (registered)
    {
        return true;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &BatchRenameDialog::WindowProc;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;

    if (::RegisterClassExW(&wc) == 0)
    {
        return false;
    }

    registered = true;
    return true;
}

LRESULT CALLBACK BatchRenameDialog::WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    BatchRenameDialog* self =
        reinterpret_cast<BatchRenameDialog*>(::GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE)
    {
        CREATESTRUCTW* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<BatchRenameDialog*>(createStruct->lpCreateParams);

        if (self != nullptr)
        {
            self->m_hWnd = hWnd;
            ::SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
    }

    if (self == nullptr)
    {
        return ::DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    return self->HandleMessage(hWnd, msg, wParam, lParam);
}

bool BatchRenameDialog::ShowModal(HWND owner, const std::vector<std::wstring>& paths,
                                  std::vector<BatchRenameResult>& results)
{
    m_hOwner = owner;

    // ---- 把路径变成带大小/时间的条目（排序和预览都要用）----
    m_items.clear();

    for (const auto& path : paths)
    {
        BatchRenameItem item = {};
        item.path = path;

        // 文件名：最后一个反斜杠之后
        const size_t slash = path.find_last_of(L'\\');
        item.name = (slash == std::wstring::npos) ? path : path.substr(slash + 1);

        // 顺手查一下属性：是不是文件夹、多大、什么时候改的（排序用；查不到就算了）
        WIN32_FILE_ATTRIBUTE_DATA attributeData = {};
        if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributeData))
        {
            item.isFolder = (attributeData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            item.size     = (static_cast<uint64_t>(attributeData.nFileSizeHigh) << 32) |
                            attributeData.nFileSizeLow;
            item.modified = attributeData.ftLastWriteTime;
        }

        if (!item.name.empty())
        {
            m_items.push_back(item);
        }
    }

    if (m_items.size() < 2)
    {
        return false;
    }

    if (!CreateDialogWindow())
    {
        return false;
    }

    // ---- 模态循环（和「设置」「打开方式」同一套写法）----
    ::EnableWindow(m_hOwner, FALSE);
    ::ShowWindow(m_hWnd, SW_SHOW);
    ::UpdateWindow(m_hWnd);

    MSG msg;
    while (::IsWindow(m_hWnd))
    {
        if (::GetMessageW(&msg, nullptr, 0, 0) <= 0)
        {
            if (msg.message == WM_QUIT)
            {
                ::PostQuitMessage(static_cast<int>(msg.wParam));
            }
            break;
        }

        if (!::IsDialogMessageW(m_hWnd, &msg))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }

    ::EnableWindow(m_hOwner, TRUE);
    ::SetForegroundWindow(m_hOwner);

    // ---- 回传结果 ----
    results.clear();

    if (!m_applied)
    {
        return false;
    }

    for (const auto& row : m_rows)
    {
        BatchRenameResult result = {};
        result.oldPath = m_items[row.itemIndex].path;
        result.newName = row.newName;
        results.push_back(result);
    }

    return true;
}

// ============================================================================
// 建窗口与控件
// ============================================================================

bool BatchRenameDialog::CreateDialogWindow()
{
    if (!EnsureClassRegistered())
    {
        return false;
    }

    if (m_whiteBrush == nullptr)
    {
        m_whiteBrush = ::CreateSolidBrush(kBackground);
    }

    // ---- 字体 ----
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);

    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
    {
        m_font = ::CreateFontIndirectW(&metrics.lfMessageFont);

        LOGFONTW titleFont = metrics.lfMessageFont;
        titleFont.lfWeight = FW_SEMIBOLD;
        titleFont.lfHeight = static_cast<LONG>(titleFont.lfHeight * 1.25);
        m_fontTitle = ::CreateFontIndirectW(&titleFont);

        LOGFONTW smallFont = metrics.lfMessageFont;
        smallFont.lfHeight = static_cast<LONG>(smallFont.lfHeight * 0.92);
        m_fontSmall = ::CreateFontIndirectW(&smallFont);
    }

    if (m_font == nullptr)
    {
        m_font = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    }
    if (m_fontTitle == nullptr)
    {
        m_fontTitle = m_font;
    }
    if (m_fontSmall == nullptr)
    {
        m_fontSmall = m_font;
    }

    // ---- 按客户区反推窗口大小，居中于主窗口 ----
    int dpi = ::GetDpiForWindow(m_hOwner != nullptr ? m_hOwner : ::GetDesktopWindow());
    if (dpi <= 0)
    {
        dpi = 96;
    }

    RECT windowRect = { 0, 0, ::MulDiv(kClientWidth, dpi, 96), ::MulDiv(kClientHeight, dpi, 96) };
    ::AdjustWindowRectEx(&windowRect, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);

    const int width  = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;

    RECT ownerRect = {};
    if (m_hOwner != nullptr)
    {
        ::GetWindowRect(m_hOwner, &ownerRect);
    }

    int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    // 别把窗口顶到屏幕外面
    RECT workArea = {};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);

    if (x < workArea.left)
    {
        x = workArea.left;
    }
    if (y < workArea.top)
    {
        y = workArea.top;
    }
    if (x + width > workArea.right)
    {
        x = workArea.right - width;
    }
    if (y + height > workArea.bottom)
    {
        y = workArea.bottom - height;
    }

    m_hWnd = ::CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                               kClassName,
                               L"批量重命名",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               x, y, width, height,
                               m_hOwner,
                               nullptr,
                               ::GetModuleHandleW(nullptr),
                               this);

    if (m_hWnd == nullptr)
    {
        Logger::Error(L"批量重命名窗口创建失败");
        return false;
    }

    CreateControls();
    Layout();

    // 先算一遍预览，用户一打开就能看到效果
    Recalculate();

    // 光标落在第一个输入框，直接就能开始打字
    if (m_hPrefix != nullptr)
    {
        ::SetFocus(m_hPrefix);
    }

    return true;
}

void BatchRenameDialog::CreateControls()
{
    // 通用控件（列表要用）——重复初始化没有副作用
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES;
    ::InitCommonControlsEx(&icc);

    const HINSTANCE instance = ::GetModuleHandleW(nullptr);

    // ---- 预览列表（report 模式：原名 / 新名 / 状态）----
    m_hList = ::CreateWindowExW(0, WC_LISTVIEWW, L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                0, 0, 0, 0,
                                m_hWnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdList)),
                                instance, nullptr);

    if (m_hList != nullptr)
    {
        ::SendMessageW(m_hList, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        ::SendMessageW(m_hList, LVM_SETEXTENDEDLISTVIEWSTYLE,
                       LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
                       LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        struct ColumnSpec
        {
            const wchar_t* text;
            int            width;   // 占列表内宽的百分比
        };

        const ColumnSpec columns[] =
        {
            { L"原文件名", 42 },
            { L"新文件名", 42 },
            { L"状态",     16 }
        };

        for (int i = 0; i < static_cast<int>(_countof(columns)); ++i)
        {
            LVCOLUMNW column = {};
            column.mask     = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            column.fmt      = LVCFMT_LEFT;
            column.pszText  = const_cast<wchar_t*>(columns[i].text);
            column.cx       = 200;   // Layout 里会按真实宽度再算一次
            ListView_InsertColumn(m_hList, i, &column);
        }
    }

    // ---- 左栏：名称类 ----
    auto createEdit = [&](int id, const wchar_t* cue) -> HWND
    {
        HWND edit = ::CreateWindowExW(0, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                      ES_AUTOHSCROLL | WS_BORDER,
                                      0, 0, 0, 0, m_hWnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                      instance, nullptr);
        if (edit != nullptr)
        {
            ::SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
            ::SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
        }
        return edit;
    };

    m_hPrefix  = createEdit(kIdPrefix,  L"给名字前面加的内容");
    m_hSuffix  = createEdit(kIdSuffix,  L"给名字后面加的内容（扩展名前）");
    m_hFind    = createEdit(kIdFind,    L"要在名字里查找的文字");
    m_hReplace = createEdit(kIdReplace, L"替换成什么（留空＝删掉查到的文字）");

    m_hCaseSensitive = ::CreateWindowExW(0, L"BUTTON", L"区分大小写",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                         0, 0, 0, 0, m_hWnd,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCaseSensitive)),
                                         instance, nullptr);
    if (m_hCaseSensitive != nullptr)
    {
        ::SendMessageW(m_hCaseSensitive, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
    }

    // ---- 右栏：序号 ----
    m_hNumberEnable = ::CreateWindowExW(0, L"BUTTON", L"加序号",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                        0, 0, 0, 0, m_hWnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdNumberEnable)),
                                        instance, nullptr);
    if (m_hNumberEnable != nullptr)
    {
        ::SendMessageW(m_hNumberEnable, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
    }

    m_hNumberStart  = createEdit(kIdNumberStart,  L"1");
    m_hNumberStep   = createEdit(kIdNumberStep,   L"1");
    m_hNumberDigits = createEdit(kIdNumberDigits, L"2");

    // 数字框只收数字
    for (HWND edit : { m_hNumberStart, m_hNumberStep, m_hNumberDigits })
    {
        if (edit != nullptr)
        {
            ::SendMessageW(edit, EM_SETLIMITTEXT, 6, 0);
        }
    }

    // 位置：前缀 / 后缀
    m_hNumberPos = ::CreateWindowExW(0, L"COMBOBOX", L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                     CBS_DROPDOWNLIST | WS_VSCROLL,
                                     0, 0, 0, 0, m_hWnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdNumberPos)),
                                     instance, nullptr);
    if (m_hNumberPos != nullptr)
    {
        ::SendMessageW(m_hNumberPos, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        ::SendMessageW(m_hNumberPos, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"放在名字后面"));
        ::SendMessageW(m_hNumberPos, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"放在名字前面"));
        ::SendMessageW(m_hNumberPos, CB_SETCURSEL, 0, 0);
    }

    m_hNumberSep = createEdit(kIdNumberSep, L"-");

    // ---- 扩展名 ----
    m_hExtMode = ::CreateWindowExW(0, L"COMBOBOX", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                   CBS_DROPDOWNLIST | WS_VSCROLL,
                                   0, 0, 0, 0, m_hWnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdExtMode)),
                                   instance, nullptr);
    if (m_hExtMode != nullptr)
    {
        ::SendMessageW(m_hExtMode, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        ::SendMessageW(m_hExtMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"保留原扩展名"));
        ::SendMessageW(m_hExtMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"改成指定扩展名"));
        ::SendMessageW(m_hExtMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"去掉扩展名"));
        ::SendMessageW(m_hExtMode, CB_SETCURSEL, 0, 0);
    }

    m_hExtValue = createEdit(kIdExtValue, L"txt");

    // ---- 大小写 ----
    m_hCaseMode = ::CreateWindowExW(0, L"COMBOBOX", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    CBS_DROPDOWNLIST | WS_VSCROLL,
                                    0, 0, 0, 0, m_hWnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCaseMode)),
                                    instance, nullptr);
    if (m_hCaseMode != nullptr)
    {
        ::SendMessageW(m_hCaseMode, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        ::SendMessageW(m_hCaseMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"保持原样"));
        ::SendMessageW(m_hCaseMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"全部大写"));
        ::SendMessageW(m_hCaseMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"全部小写"));
        ::SendMessageW(m_hCaseMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"首字母大写"));
        ::SendMessageW(m_hCaseMode, CB_SETCURSEL, 0, 0);
    }

    // ---- 排序 ----
    m_hSortMode = ::CreateWindowExW(0, L"COMBOBOX", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    CBS_DROPDOWNLIST | WS_VSCROLL,
                                    0, 0, 0, 0, m_hWnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSortMode)),
                                    instance, nullptr);
    if (m_hSortMode != nullptr)
    {
        ::SendMessageW(m_hSortMode, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        ::SendMessageW(m_hSortMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"按文件名"));
        ::SendMessageW(m_hSortMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"按修改时间"));
        ::SendMessageW(m_hSortMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"按文件大小"));
        ::SendMessageW(m_hSortMode, CB_SETCURSEL, 0, 0);
    }

    m_hSortReverse = ::CreateWindowExW(0, L"BUTTON", L"倒序",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                       0, 0, 0, 0, m_hWnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSortReverse)),
                                       instance, nullptr);
    if (m_hSortReverse != nullptr)
    {
        ::SendMessageW(m_hSortReverse, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
    }

    EnableRuleControls();
}

void BatchRenameDialog::Layout()
{
    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);

    const int padding = Scale(kPadding);

    // ---- 预览列表 ----
    if (m_hList != nullptr)
    {
        const int listWidth = clientRect.right - padding * 2;

        ::MoveWindow(m_hList, padding, Scale(kListTop), listWidth, Scale(kListHeight), TRUE);

        // 列宽按真实内宽重算：原名 42% / 新名 42% / 状态 16%
        const int innerWidth = listWidth - Scale(4);
        ListView_SetColumnWidth(m_hList, 0, innerWidth * 42 / 100);
        ListView_SetColumnWidth(m_hList, 1, innerWidth * 42 / 100);
        ListView_SetColumnWidth(m_hList, 2, innerWidth * 16 / 100);
    }

    // ---- 左栏 5 行：前缀 / 后缀 / 查找 / 替换为 / 区分大小写 ----
    const int labelWidth = Scale(66);
    const int editX      = Scale(kLeftColumnX) + labelWidth + Scale(8);
    const int editWidth  = Scale(kLeftColumnW) - labelWidth - Scale(8);
    const int editHeight = Scale(24);

    auto placeRow = [&](HWND control, int row, int width)
    {
        if (control == nullptr)
        {
            return;
        }

        const int top = Scale(kRuleTop) + Scale(kRuleRowHeight) * row;
        ::MoveWindow(control, editX, top + Scale(4), width, editHeight, TRUE);
    };

    placeRow(m_hPrefix,  0, editWidth);
    placeRow(m_hSuffix,  1, editWidth);
    placeRow(m_hFind,    2, editWidth);
    placeRow(m_hReplace, 3, editWidth);

    if (m_hCaseSensitive != nullptr)
    {
        const int top = Scale(kRuleTop) + Scale(kRuleRowHeight) * 4;
        ::MoveWindow(m_hCaseSensitive, Scale(kLeftColumnX), top + Scale(5),
                     Scale(160), Scale(22), TRUE);
    }

    // ---- 右栏 ----
    const int rightX = Scale(kRightColumnX);
    const int rowY   = Scale(kRuleTop);

    // 第 0 行：加序号  起始[ ]  步长[ ]  位数[ ]
    {
        const int top = rowY + Scale(kRuleRowHeight) * 0;

        if (m_hNumberEnable != nullptr)
        {
            ::MoveWindow(m_hNumberEnable, rightX, top + Scale(5), Scale(84), Scale(22), TRUE);
        }

        const int fieldX = rightX + Scale(92);
        const int smallEditWidth = Scale(52);

        ::MoveWindow(m_hNumberStart, fieldX + Scale(32), top + Scale(4),
                     smallEditWidth, editHeight, TRUE);
        ::MoveWindow(m_hNumberStep, fieldX + Scale(120), top + Scale(4),
                     smallEditWidth, editHeight, TRUE);
        ::MoveWindow(m_hNumberDigits, fieldX + Scale(208), top + Scale(4),
                     smallEditWidth, editHeight, TRUE);
    }

    // 第 1 行：位置[▾]  分隔符[ ]
    {
        const int top = rowY + Scale(kRuleRowHeight) * 1;

        if (m_hNumberPos != nullptr)
        {
            ::MoveWindow(m_hNumberPos, rightX + Scale(56), top + Scale(4),
                         Scale(126), Scale(24 * 3), TRUE);
        }

        if (m_hNumberSep != nullptr)
        {
            ::MoveWindow(m_hNumberSep, rightX + Scale(260), top + Scale(4),
                         Scale(52), editHeight, TRUE);
        }
    }

    // 第 2 行：扩展名[▾]  改为[ ]
    {
        const int top = rowY + Scale(kRuleRowHeight) * 2;

        if (m_hExtMode != nullptr)
        {
            ::MoveWindow(m_hExtMode, rightX + Scale(56), top + Scale(4),
                         Scale(150), Scale(24 * 4), TRUE);
        }

        if (m_hExtValue != nullptr)
        {
            ::MoveWindow(m_hExtValue, rightX + Scale(258), top + Scale(4),
                         Scale(100), editHeight, TRUE);
        }
    }

    // 第 3 行：大小写[▾]
    {
        const int top = rowY + Scale(kRuleRowHeight) * 3;

        if (m_hCaseMode != nullptr)
        {
            ::MoveWindow(m_hCaseMode, rightX + Scale(56), top + Scale(4),
                         Scale(150), Scale(24 * 5), TRUE);
        }
    }

    // 第 4 行：排序[▾]  倒序
    {
        const int top = rowY + Scale(kRuleRowHeight) * 4;

        if (m_hSortMode != nullptr)
        {
            ::MoveWindow(m_hSortMode, rightX + Scale(56), top + Scale(4),
                         Scale(150), Scale(24 * 4), TRUE);
        }

        if (m_hSortReverse != nullptr)
        {
            ::MoveWindow(m_hSortReverse, rightX + Scale(216), top + Scale(5),
                         Scale(90), Scale(22), TRUE);
        }
    }

    // ---- 底部两个按钮 ----
    const int buttonWidth  = Scale(96);
    const int buttonHeight = Scale(32);

    m_applyRect.right  = clientRect.right - padding;
    m_applyRect.left   = m_applyRect.right - buttonWidth;
    m_applyRect.bottom = clientRect.bottom - Scale(16);
    m_applyRect.top    = m_applyRect.bottom - buttonHeight;

    m_cancelRect.right  = m_applyRect.left - Scale(10);
    m_cancelRect.left   = m_cancelRect.right - buttonWidth;
    m_cancelRect.bottom = m_applyRect.bottom;
    m_cancelRect.top    = m_applyRect.top;
}

// ============================================================================
// 规则区启用状态（序号没勾起来时，那几个框灰掉）
// ============================================================================
void BatchRenameDialog::EnableRuleControls()
{
    const bool numberOn = (m_hNumberEnable != nullptr) &&
                          (::SendMessageW(m_hNumberEnable, BM_GETCHECK, 0, 0) == BST_CHECKED);

    for (HWND control : { m_hNumberStart, m_hNumberStep, m_hNumberDigits,
                          m_hNumberPos, m_hNumberSep })
    {
        if (control != nullptr)
        {
            ::EnableWindow(control, numberOn ? TRUE : FALSE);
        }
    }

    const int extMode = ComboSelection(m_hExtMode);
    if (m_hExtValue != nullptr)
    {
        ::EnableWindow(m_hExtValue, extMode == 1 ? TRUE : FALSE);
    }
}

// ============================================================================
// 计算
// ============================================================================

std::wstring BatchRenameDialog::GetTextOf(HWND control) const
{
    if (control == nullptr)
    {
        return L"";
    }

    const int length = ::GetWindowTextLengthW(control);
    if (length <= 0)
    {
        return L"";
    }

    std::wstring text(static_cast<size_t>(length), L'\0');
    ::GetWindowTextW(control, text.data(), length + 1);
    return text;
}

int BatchRenameDialog::ComboSelection(HWND control) const
{
    if (control == nullptr)
    {
        return 0;
    }

    const int selected = static_cast<int>(::SendMessageW(control, CB_GETCURSEL, 0, 0));
    return (selected < 0) ? 0 : selected;
}

std::wstring BatchRenameDialog::ComputeNewName(size_t index, const std::wstring& originalName) const
{
    std::wstring stem;
    std::wstring extension;
    SplitFileName(originalName, stem, extension);

    // 1) 查找替换
    const std::wstring find = GetTextOf(m_hFind);
    if (!find.empty())
    {
        const bool caseSensitive =
            (m_hCaseSensitive != nullptr) &&
            (::SendMessageW(m_hCaseSensitive, BM_GETCHECK, 0, 0) == BST_CHECKED);

        stem = ReplaceAll(stem, find, GetTextOf(m_hReplace), caseSensitive);
    }

    // 2) 大小写
    switch (ComboSelection(m_hCaseMode))
    {
    case 1:   // 全部大写
        ::CharUpperBuffW(stem.data(), static_cast<DWORD>(stem.size()));
        break;

    case 2:   // 全部小写
        ::CharLowerBuffW(stem.data(), static_cast<DWORD>(stem.size()));
        break;

    case 3:   // 首字母大写（其余转小写）
        if (!stem.empty())
        {
            ::CharLowerBuffW(stem.data(), static_cast<DWORD>(stem.size()));
            stem[0] = static_cast<wchar_t>(::towupper(stem[0]));
        }
        break;

    default:
        break;
    }

    // 3) 序号（只有勾了才加；编号按排序后的次序给）
    const bool numberOn = (m_hNumberEnable != nullptr) &&
                          (::SendMessageW(m_hNumberEnable, BM_GETCHECK, 0, 0) == BST_CHECKED);

    if (numberOn)
    {
        const long long start  = ReadInteger(m_hNumberStart, 1, 0);
        const long long step   = ReadInteger(m_hNumberStep, 1, 1);
        const int       digits = static_cast<int>(ReadInteger(m_hNumberDigits, 1, 1));

        const std::wstring numberText =
            FormatNumber(start + step * static_cast<long long>(index), digits);
        const std::wstring separator = GetTextOf(m_hNumberSep);

        if (ComboSelection(m_hNumberPos) == 1)
        {
            stem = numberText + separator + stem;     // 放在名字前面
        }
        else
        {
            stem = stem + separator + numberText;     // 放在名字后面
        }
    }

    // 4) 前缀 / 后缀（加在整理好的主名两端）
    stem = GetTextOf(m_hPrefix) + stem + GetTextOf(m_hSuffix);

    // 5) 扩展名
    switch (ComboSelection(m_hExtMode))
    {
    case 1:   // 改成指定扩展名
    {
        std::wstring value = GetTextOf(m_hExtValue);
        if (!value.empty())
        {
            if (value[0] != L'.')
            {
                value.insert(value.begin(), L'.');
            }
            extension = value;
        }
        break;
    }

    case 2:   // 去掉扩展名
        extension.clear();
        break;

    default:  // 保留原扩展名
        break;
    }

    return stem + extension;
}

void BatchRenameDialog::BuildPreviewRows()
{
    m_rows.clear();

    // ---- 先按排序规则排出"谁先谁后"，序号就按这个次序给 ----
    std::vector<size_t> order(m_items.size());
    for (size_t i = 0; i < order.size(); ++i)
    {
        order[i] = i;
    }

    const int sortMode = ComboSelection(m_hSortMode);

    const bool reverseOn = (m_hSortReverse != nullptr) &&
                           (::SendMessageW(m_hSortReverse, BM_GETCHECK, 0, 0) == BST_CHECKED);

    std::stable_sort(order.begin(), order.end(),
                     [&](size_t left, size_t right)
                     {
                         const BatchRenameItem& a = m_items[left];
                         const BatchRenameItem& b = m_items[right];

                         bool less = false;

                         switch (sortMode)
                         {
                         case 1:   // 修改时间（早的排前面）
                             less = (::CompareFileTime(&a.modified, &b.modified) < 0);
                             break;

                         case 2:   // 文件大小（小的排前面）
                             less = (a.size < b.size);
                             break;

                         default:  // 文件名（不分大小写）
                             less = (::_wcsicmp(a.name.c_str(), b.name.c_str()) < 0);
                             break;
                         }

                         return less;
                     });

    if (reverseOn)
    {
        std::reverse(order.begin(), order.end());
    }

    // ---- 逐项算新名字 ----
    std::vector<std::wstring> newNames(m_items.size());

    for (size_t i = 0; i < order.size(); ++i)
    {
        newNames[order[i]] = ComputeNewName(i, m_items[order[i]].name);
    }

    // ---- 状态判定：空名 / 非法字符 / 重名 / 已修改 / 未改动 ----
    for (size_t i = 0; i < order.size(); ++i)
    {
        const size_t itemIndex = order[i];

        PreviewRow row = {};
        row.itemIndex = itemIndex;
        row.oldName = m_items[itemIndex].name;
        row.newName = newNames[itemIndex];

        if (row.newName.empty())
        {
            row.state     = 2;
            row.stateText = L"名字为空";
        }
        else if (row.newName.find_first_of(kInvalidChars) != std::wstring::npos)
        {
            row.state     = 2;
            row.stateText = L"含非法字符";
        }
        else
        {
            // 和别的项撞名了吗（不区分大小写，和文件系统一致）
            const std::wstring lowered = ToLowerText(row.newName);
            bool               collision = false;

            for (size_t j = 0; j < order.size(); ++j)
            {
                if (j == i)
                {
                    continue;
                }

                if (ToLowerText(newNames[order[j]]) == lowered)
                {
                    collision = true;
                    break;
                }
            }

            if (collision)
            {
                row.state     = 2;
                row.stateText = L"和别的项重名";
            }
            else if (row.newName == row.oldName)
            {
                row.state     = 0;
                row.stateText = L"未改动";
            }
            else
            {
                row.state     = 1;
                row.stateText = L"已修改";
            }
        }

        m_rows.push_back(row);
    }
}

void BatchRenameDialog::FillPreviewList()
{
    if (m_hList == nullptr)
    {
        return;
    }

    ::SendMessageW(m_hList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(m_hList);

    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i)
    {
        const PreviewRow& row = m_rows[static_cast<size_t>(i)];

        LVITEMW item = {};
        item.mask     = LVIF_TEXT;
        item.iItem    = i;
        item.pszText  = const_cast<wchar_t*>(row.oldName.c_str());
        ListView_InsertItem(m_hList, &item);

        ListView_SetItemText(m_hList, i, 1, const_cast<wchar_t*>(row.newName.c_str()));
        ListView_SetItemText(m_hList, i, 2, const_cast<wchar_t*>(row.stateText.c_str()));
    }

    ::SendMessageW(m_hList, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(m_hList, nullptr, TRUE);
}

void BatchRenameDialog::Recalculate()
{
    // 控件还没建好时（建窗口过程中 SetWindowText 会触发 EN_CHANGE）直接跳过
    if (m_hList == nullptr || m_items.empty())
    {
        return;
    }

    EnableRuleControls();
    BuildPreviewRows();
    FillPreviewList();

    // 底部统计变了，重画那块
    if (m_hWnd != nullptr)
    {
        ::InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

// ============================================================================
// 画窗口
// ============================================================================

void BatchRenameDialog::DrawButton(HDC dc, const RECT& rect, const wchar_t* text,
                                   bool hot, bool pressed, bool primary)
{
    COLORREF face   = kButtonFace;
    COLORREF border = kButtonBorder;

    if (primary)
    {
        face   = pressed ? kPrimaryDownFace : (hot ? kPrimaryHotFace : kPrimaryFace);
        border = kPrimaryBorder;
    }
    else if (pressed)
    {
        face   = kButtonDownFace;
        border = kButtonDownBorder;
    }
    else if (hot)
    {
        face   = kButtonHotFace;
        border = kButtonHotBorder;
    }

    // 背景与边框
    // 注意：Rectangle() 会同时用"当前画刷"刷内部，只选画笔不选画刷的话
    // 里面会被默认白刷盖掉（primary 的蓝底就是这么丢的），所以画笔和画刷一起选。
    HBRUSH brush    = ::CreateSolidBrush(face);
    HPEN   pen      = ::CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = ::SelectObject(dc, brush);
    HGDIOBJ oldPen   = ::SelectObject(dc, pen);

    ::Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);

    ::SelectObject(dc, oldPen);
    ::SelectObject(dc, oldBrush);
    ::DeleteObject(pen);
    ::DeleteObject(brush);

    // 文字
    HGDIOBJ oldFont = ::SelectObject(dc, m_font);

    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, primary ? RGB(255, 255, 255) : kTitleColor);

    RECT textRect = rect;
    ::DrawTextW(dc, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    ::SelectObject(dc, oldFont);
}

void BatchRenameDialog::Draw(HDC dc)
{
    RECT clientRect = {};
    ::GetClientRect(m_hWnd, &clientRect);

    // 白底
    ::FillRect(dc, &clientRect, m_whiteBrush);

    const int padding = Scale(kPadding);

    // ---- 标题 ----
    HGDIOBJ oldFont = ::SelectObject(dc, m_fontTitle);
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, kTitleColor);

    RECT titleRect = { padding, Scale(10), clientRect.right - padding, Scale(44) };
    ::DrawTextW(dc, kTitleText, -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // ---- 副标题（说明这窗口怎么用）----
    ::SelectObject(dc, m_fontSmall);
    ::SetTextColor(dc, kHintColor);

    RECT hintRect = { padding, Scale(44), clientRect.right - padding, Scale(66) };
    ::DrawTextW(dc, L"改下面的规则，上面的「新文件名」会立刻跟着变；确认没问题再点应用。",
                -1, &hintRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // ---- 两条分隔线 ----
    HPEN dividerPen = ::CreatePen(PS_SOLID, 1, kDividerColor);
    HGDIOBJ oldPen = ::SelectObject(dc, dividerPen);

    ::MoveToEx(dc, padding, Scale(kListTop - 6), nullptr);
    ::LineTo(dc, clientRect.right - padding, Scale(kListTop - 6));

    ::MoveToEx(dc, padding, Scale(kRuleTop - 12), nullptr);
    ::LineTo(dc, clientRect.right - padding, Scale(kRuleTop - 12));

    ::SelectObject(dc, oldPen);
    ::DeleteObject(dividerPen);

    // ---- 左栏四个行首标签 ----
    ::SelectObject(dc, m_font);
    ::SetTextColor(dc, kTitleColor);

    const wchar_t* leftLabels[] = { L"前  缀", L"后  缀", L"查  找", L"替换为" };
    const int labelWidth = Scale(66);

    for (int i = 0; i < 4; ++i)
    {
        RECT labelRect = { padding, Scale(kRuleTop) + Scale(kRuleRowHeight) * i,
                           padding + labelWidth, Scale(kRuleTop) + Scale(kRuleRowHeight) * (i + 1) };
        ::DrawTextW(dc, leftLabels[i], -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // ---- 右栏行首标签 ----
    const int rightX = Scale(kRightColumnX);

    const wchar_t* rightLabels[] = { L"", L"位  置", L"扩展名", L"大小写", L"排  序" };
    const wchar_t* numberFieldLabels[] = { L"起始", L"步长", L"位数" };

    for (int i = 0; i < 5; ++i)
    {
        if (rightLabels[i][0] == L'\0')
        {
            continue;
        }

        RECT labelRect = { rightX, Scale(kRuleTop) + Scale(kRuleRowHeight) * i,
                           rightX + Scale(56), Scale(kRuleTop) + Scale(kRuleRowHeight) * (i + 1) };
        ::DrawTextW(dc, rightLabels[i], -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // 序号那三个小标签（起点位置和 Layout 里一致：每组间隔 88，标签在框左边 32 宽）
    {
        const int fieldX = rightX + Scale(92);
        const int width  = Scale(32);

        for (int i = 0; i < 3; ++i)
        {
            RECT labelRect = { fieldX + Scale(88) * i, Scale(kRuleTop),
                               fieldX + Scale(88) * i + width,
                               Scale(kRuleTop) + Scale(kRuleRowHeight) };
            ::DrawTextW(dc, numberFieldLabels[i], -1, &labelRect,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        // 「分隔符」在第二行右侧
        RECT sepLabel = { rightX + Scale(196), Scale(kRuleTop) + Scale(kRuleRowHeight),
                          rightX + Scale(256), Scale(kRuleTop) + Scale(kRuleRowHeight) * 2 };
        ::DrawTextW(dc, L"分隔符", -1, &sepLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // ---- 底部统计 ----
    int changedCount = 0;
    int conflictCount = 0;

    for (const auto& row : m_rows)
    {
        if (row.state == 1)
        {
            ++changedCount;
        }
        else if (row.state == 2)
        {
            ++conflictCount;
        }
    }

    ::SelectObject(dc, m_fontSmall);

    wchar_t statsText[256] = {};
    ::wsprintfW(statsText, L"共 %u 项 · 会改动 %u 项 · 冲突 %u 项",
                static_cast<unsigned>(m_rows.size()),
                static_cast<unsigned>(changedCount),
                static_cast<unsigned>(conflictCount));

    ::SetTextColor(dc, (conflictCount > 0) ? kErrorColor : kHintColor);

    RECT statsRect = { padding, m_applyRect.top - Scale(6) - Scale(20),
                       m_cancelRect.left - Scale(16), m_applyRect.top - Scale(6) };
    ::DrawTextW(dc, statsText, -1, &statsRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 有冲突时「应用」不可用，明说原因
    if (conflictCount > 0)
    {
        RECT warnRect = { padding, m_applyRect.top - Scale(6) - Scale(20) + Scale(18),
                          m_cancelRect.left - Scale(16), m_applyRect.top - Scale(6) + Scale(18) };
        ::DrawTextW(dc, L"有冲突的行会标红，改到没有冲突才能应用。", -1, &warnRect,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    ::SelectObject(dc, oldFont);

    // ---- 两个按钮 ----
    DrawButton(dc, m_cancelRect, L"取消", m_hotCancel, m_pressedCancel, false);
    DrawButton(dc, m_applyRect,  L"应用", m_hotApply,  m_pressedApply,  true);
}

// ============================================================================
// 消息处理
// ============================================================================

LRESULT BatchRenameDialog::HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT paintStruct = {};
        HDC dc = ::BeginPaint(hWnd, &paintStruct);
        Draw(dc);
        ::EndPaint(hWnd, &paintStruct);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;   // 背景在 WM_PAINT 里画，别闪

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, kTitleColor);
        return reinterpret_cast<LRESULT>(m_whiteBrush);
    }

    case WM_COMMAND:
    {
        const int controlId = LOWORD(wParam);
        const int notification = HIWORD(wParam);

        // 回车 / Esc（IsDialogMessage 会翻译成 IDOK / IDCANCEL）
        if (controlId == IDCANCEL)
        {
            ::DestroyWindow(hWnd);
            return 0;
        }

        if (controlId == IDOK)
        {
            ::SendMessageW(hWnd, WM_COMMAND,
                           MAKEWPARAM(kIdApply, BN_CLICKED), 0);
            return 0;
        }

        // 自绘按钮：不是真实窗口，靠 ID 判断（lParam 为 0）
        if (lParam == 0)
        {
            if (controlId == kIdCancel)
            {
                ::DestroyWindow(hWnd);
                return 0;
            }

            if (controlId == kIdApply)
            {
                // 有冲突就不让应用
                bool hasConflict = false;
                for (const auto& row : m_rows)
                {
                    if (row.state == 2)
                    {
                        hasConflict = true;
                        break;
                    }
                }

                if (!hasConflict && !m_rows.empty())
                {
                    m_applied = true;
                    ::DestroyWindow(hWnd);
                }
                return 0;
            }
        }

        // 规则控件的任何改动 → 重算预览
        if (controlId >= kIdPrefix && controlId <= kIdSortReverse)
        {
            if (notification == EN_CHANGE || notification == CBN_SELCHANGE ||
                notification == BN_CLICKED)
            {
                Recalculate();
            }
            return 0;
        }

        return 0;
    }

    case WM_DRAWITEM:
        return 0;

    case WM_NOTIFY:
    {
        NMHDR* header = reinterpret_cast<NMHDR*>(lParam);

        if (header != nullptr && header->idFrom == kIdList &&
            header->code == NM_CUSTOMDRAW)
        {
            NMLVCUSTOMDRAW* customDraw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);

            switch (customDraw->nmcd.dwDrawStage)
            {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;

            case CDDS_ITEMPREPAINT:
            {
                const size_t rowIndex = static_cast<size_t>(customDraw->nmcd.dwItemSpec);

                if (rowIndex < m_rows.size())
                {
                    switch (m_rows[rowIndex].state)
                    {
                    case 2:   // 冲突：红字
                        ::SetTextColor(customDraw->nmcd.hdc, kErrorColor);
                        return CDRF_NEWFONT;

                    case 1:   // 已修改：正常深色
                        ::SetTextColor(customDraw->nmcd.hdc, kTitleColor);
                        return CDRF_NEWFONT;

                    default:  // 未改动：浅灰
                        ::SetTextColor(customDraw->nmcd.hdc, kHintColor);
                        return CDRF_NEWFONT;
                    }
                }

                return CDRF_DODEFAULT;
            }

            default:
                break;
            }
        }

        return 0;
    }

    case WM_MOUSEMOVE:
    {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);

        const bool overCancel = (x >= m_cancelRect.left && x < m_cancelRect.right &&
                                 y >= m_cancelRect.top  && y < m_cancelRect.bottom);
        const bool overApply  = (x >= m_applyRect.left && x < m_applyRect.right &&
                                 y >= m_applyRect.top  && y < m_applyRect.bottom);

        if (overCancel != m_hotCancel || overApply != m_hotApply)
        {
            m_hotCancel = overCancel;
            m_hotApply  = overApply;
            ::InvalidateRect(hWnd, nullptr, FALSE);
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

        // 鼠标变手型，看着能点
        if (m_hotCancel || m_hotApply)
        {
            ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
        }

        return 0;
    }

    case WM_MOUSELEAVE:
        m_trackingLeave = false;

        if (m_hotCancel || m_hotApply)
        {
            m_hotCancel = false;
            m_hotApply  = false;
            ::InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN:
    {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);

        const bool overCancel = (x >= m_cancelRect.left && x < m_cancelRect.right &&
                                 y >= m_cancelRect.top  && y < m_cancelRect.bottom);
        const bool overApply  = (x >= m_applyRect.left && x < m_applyRect.right &&
                                 y >= m_applyRect.top  && y < m_applyRect.bottom);

        m_pressedCancel = overCancel;
        m_pressedApply  = overApply;

        if (overCancel || overApply)
        {
            ::SetCapture(hWnd);
            ::InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);

        if (::GetCapture() == hWnd)
        {
            ::ReleaseCapture();
        }

        const bool overCancel = (x >= m_cancelRect.left && x < m_cancelRect.right &&
                                 y >= m_cancelRect.top  && y < m_cancelRect.bottom);
        const bool overApply  = (x >= m_applyRect.left && x < m_applyRect.right &&
                                 y >= m_applyRect.top  && y < m_applyRect.bottom);

        const bool wasCancel = m_pressedCancel;
        const bool wasApply  = m_pressedApply;

        m_pressedCancel = false;
        m_pressedApply  = false;
        ::InvalidateRect(hWnd, nullptr, FALSE);

        if (wasCancel && overCancel)
        {
            ::SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(kIdCancel, BN_CLICKED), 0);
        }
        else if (wasApply && overApply)
        {
            ::SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(kIdApply, BN_CLICKED), 0);
        }
        return 0;
    }

    case WM_CLOSE:
        ::DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        m_hWnd = nullptr;
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
