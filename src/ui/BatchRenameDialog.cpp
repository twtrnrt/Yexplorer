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
#include <regex>
#include <stdio.h>
#include <stdlib.h>

#include "ui/BatchRenameDialog.h"

#include "core/Logger.h"

namespace
{

const wchar_t* kClassName = L"FileExplorerBatchRename";

// 客户区大小（96 DPI 下的逻辑尺寸）
const int kClientWidth  = 940;
const int kClientHeight = 676;
const int kPadding      = 20;

// 预览列表的位置
const int kListTop    = 78;
const int kListHeight = 246;

// 模板栏（预览列表和规则区之间那一条）
const int kTemplateTop = 342;

// 规则区：两栏，每栏 5 行
const int kRuleTop      = 398;
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

    kIdRegex         = 5017,   // 查找按正则处理
    kIdRegexPreset   = 5018,   // 常用正则下拉

    kIdTemplateCombo  = 5020,  // 模板名（可编辑）
    kIdTemplateLoad   = 5021,  // 载入模板（自绘按钮）
    kIdTemplateSave   = 5022,  // 保存模板（自绘按钮）
    kIdTemplateDelete = 5023,  // 删除模板（自绘按钮）

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
// 正则：编译一次缓存起来
//
// 每敲一个字符都要把整张表重算一遍，如果每次都现场编译正则，
// 几百项的时候会明显卡；按"写法变了才重编"缓存一下就没这个问题。
// 窗口是模态的、同一时间只有一个，所以用文件级静态变量足够。
// ============================================================================

namespace
{

struct RegexCache
{
    std::wstring pattern;
    bool         valid    = true;    // 当前写法能不能编译
    bool         compiled = false;   // pattern 是不是已经编译过
    std::wregex  regex;
};

RegexCache g_regexCache;

// 编译（写法变了才真编）。返回 false 表示写法有错
bool EnsureRegexCompiled(const std::wstring& pattern)
{
    if (g_regexCache.compiled && g_regexCache.pattern == pattern)
    {
        return g_regexCache.valid;
    }

    g_regexCache.pattern  = pattern;
    g_regexCache.compiled = true;
    g_regexCache.valid    = true;

    try
    {
        g_regexCache.regex = std::wregex(pattern, std::regex::ECMAScript | std::regex::optimize);
    }
    catch (const std::regex_error&)
    {
        // 写法不合法：先把 valid 标掉，等用户改对了再重编
        g_regexCache.valid = false;
    }

    return g_regexCache.valid;
}

// UTF-8 读写小工具（板蓝根式的手写转换，不引第三方库）
std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return std::string();
    }

    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return std::string();
    }

    std::string result(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                          result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty())
    {
        return std::wstring();
    }

    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0)
    {
        return std::wstring();
    }

    std::wstring result(static_cast<size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                          result.data(), size);
    return result;
}

// %APPDATA%\FileExplorer\rename_templates.txt
// 和主配置放同一个目录（备份/排查都方便），但是单独一个文件：
// 模板是这一个窗口自己的东西，混进 settings.json 会让那份手写解析变复杂。
std::wstring TemplateFilePath()
{
    wchar_t appData[MAX_PATH] = {};

    if (::GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH) == 0)
    {
        return std::wstring();
    }

    std::wstring directory = std::wstring(appData) + L"\\FileExplorer";
    ::CreateDirectoryW(directory.c_str(), nullptr);

    return directory + L"\\rename_templates.txt";
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
    LoadTemplatesFromDisk();

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
    // 允许多选：可以按住 Ctrl 挑几行，再用 Ctrl+拖动 把它们整块挪位置（手动排序）
    m_hList = ::CreateWindowExW(0, WC_LISTVIEWW, L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                LVS_REPORT | LVS_SHOWSELALWAYS,
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

    // 「查找」按正则处理
    m_hRegex = ::CreateWindowExW(0, L"BUTTON", L"查找按正则处理",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                 0, 0, 0, 0, m_hWnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRegex)),
                                 instance, nullptr);
    if (m_hRegex != nullptr)
    {
        ::SendMessageW(m_hRegex, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
    }

    // 常用正则下拉：选中一条就把写法填进「查找」并勾上正则，省得自己敲
    m_hRegexPreset = ::CreateWindowExW(0, L"COMBOBOX", L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                       CBS_DROPDOWNLIST | WS_VSCROLL,
                                       0, 0, 0, 0, m_hWnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRegexPreset)),
                                       instance, nullptr);
    if (m_hRegexPreset != nullptr)
    {
        ::SendMessageW(m_hRegexPreset, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);

        for (size_t i = 0; i < RegexPresetCount(); ++i)
        {
            ::SendMessageW(m_hRegexPreset, CB_ADDSTRING, 0,
                           reinterpret_cast<LPARAM>(RegexPresetName(i)));
        }

        ::SendMessageW(m_hRegexPreset, CB_SETCURSEL, 0, 0);
    }

    // ---- 模板栏：模板名下拉（可编辑，输新名字就是新建）+ 载入 / 保存 / 删除 ----
    m_hTemplateCombo = ::CreateWindowExW(0, L"COMBOBOX", L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                         CBS_DROPDOWN | WS_VSCROLL,
                                         0, 0, 0, 0, m_hWnd,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdTemplateCombo)),
                                         instance, nullptr);
    if (m_hTemplateCombo != nullptr)
    {
        ::SendMessageW(m_hTemplateCombo, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        ::SendMessageW(m_hTemplateCombo, CB_LIMITTEXT, 40, 0);
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
        // 第 4 项：手动拖出来的顺序（在预览表里 Ctrl+拖动 之后会自动切到这个）
        ::SendMessageW(m_hSortMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"自定义顺序"));
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

    // ---- 左栏 5 行：前缀 / 后缀 / 查找（右侧带常用正则下拉）/ 替换为 / 两个复选框 ----
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

    // 「查找」这一行右边挤出 132 放常用正则下拉（查找框相应变窄）
    const int presetWidth = Scale(132);
    placeRow(m_hFind,     2, editWidth - presetWidth - Scale(6));
    placeRow(m_hReplace,  3, editWidth);

    if (m_hRegexPreset != nullptr)
    {
        const int top = Scale(kRuleTop) + Scale(kRuleRowHeight) * 2;
        ::MoveWindow(m_hRegexPreset,
                     editX + editWidth - presetWidth, top + Scale(4),
                     presetWidth, Scale(24 * 9), TRUE);
    }

    if (m_hCaseSensitive != nullptr)
    {
        const int top = Scale(kRuleTop) + Scale(kRuleRowHeight) * 4;
        ::MoveWindow(m_hCaseSensitive, Scale(kLeftColumnX), top + Scale(5),
                     Scale(130), Scale(22), TRUE);
    }

    if (m_hRegex != nullptr)
    {
        const int top = Scale(kRuleTop) + Scale(kRuleRowHeight) * 4;
        ::MoveWindow(m_hRegex, Scale(kLeftColumnX) + Scale(140), top + Scale(5),
                     Scale(150), Scale(22), TRUE);
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

    // ---- 模板栏：模板名下拉 + 载入 / 保存 / 删除 ----
    {
        const int top = Scale(kTemplateTop);

        if (m_hTemplateCombo != nullptr)
        {
            ::MoveWindow(m_hTemplateCombo,
                         Scale(kLeftColumnX) + Scale(48), top,
                         Scale(220), Scale(24 * 8), TRUE);
        }

        const int templateButtonWidth  = Scale(88);
        const int templateButtonHeight = Scale(28);

        m_templateLoadRect.left   = Scale(kLeftColumnX) + Scale(48) + Scale(226);
        m_templateLoadRect.top    = top - Scale(2);
        m_templateLoadRect.right  = m_templateLoadRect.left + templateButtonWidth;
        m_templateLoadRect.bottom = m_templateLoadRect.top + templateButtonHeight;

        m_templateSaveRect.left   = m_templateLoadRect.right + Scale(8);
        m_templateSaveRect.top    = m_templateLoadRect.top;
        m_templateSaveRect.right  = m_templateSaveRect.left + templateButtonWidth;
        m_templateSaveRect.bottom = m_templateLoadRect.bottom;

        m_templateDeleteRect.left   = m_templateSaveRect.right + Scale(8);
        m_templateDeleteRect.top    = m_templateLoadRect.top;
        m_templateDeleteRect.right  = m_templateDeleteRect.left + templateButtonWidth;
        m_templateDeleteRect.bottom = m_templateLoadRect.bottom;
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

    // 底部统计文字（单独一块，改规则时只重画这里，避免整窗闪）
    m_statsRect.left   = padding;
    m_statsRect.right  = m_cancelRect.left - Scale(12);
    m_statsRect.bottom = m_applyRect.top - Scale(4);
    m_statsRect.top    = m_statsRect.bottom - Scale(22);
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
// 常用正则预设
//
// 用户不太可能背得住正则语法，所以内置一批"日常改名真用得上"的写法，
// 下拉里选一条就把写法填进「查找」并勾上正则，改一下替换为就能用。
// 第 0 项是"不用"，后面才是真正的写法。
// ============================================================================

size_t BatchRenameDialog::RegexPresetCount()
{
    return 9;
}

const wchar_t* BatchRenameDialog::RegexPresetName(size_t index)
{
    static const wchar_t* names[] =
    {
        L"（不用，普通查找）",
        L"连续数字  \\d+",
        L"副本编号 (N)",
        L"开头编号 01_",
        L"连续空白  \\s+",
        L"8 位日期",
        L"半角括号备注",
        L"全角方括号备注",
        L"版本号 x.y.z"
    };

    const size_t count = RegexPresetCount();
    return names[(index < count) ? index : 0];
}

const wchar_t* BatchRenameDialog::RegexPresetPattern(size_t index)
{
    static const wchar_t* patterns[] =
    {
        L"",                    // 0：不用（空写法 = 不做替换）
        L"\\d+",
        L"\\s*\\(\\d+\\)$",
        L"^\\d+[\\.\\-_ ]*",
        L"\\s+",
        L"\\d{8}",
        L"\\([^)]*\\)",
        L"【[^】]*】",
        L"\\d+\\.\\d+(\\.\\d+)*"
    };

    const size_t count = RegexPresetCount();
    return patterns[(index < count) ? index : 0];
}

// ============================================================================
// 模板：读 / 写 / 收 / 填
//
// 存的文件是极简的 key=value 文本（不叫 JSON，免得和主配置的解析混在一起）：
//   一行一个字段，字段值里可以带等号；模板之间用一行 --- 隔开。
// 这样用户拿记事本打开也能看懂、能手工改。
// ============================================================================

void BatchRenameDialog::LoadTemplatesFromDisk()
{
    m_templates.clear();

    const std::wstring path = TemplateFilePath();
    if (path.empty())
    {
        return;
    }

    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr)
    {
        return;   // 第一次用，还没有这个文件，算正常
    }

    std::string bytes;
    char        buffer[4096];
    size_t      readSize = 0;

    while ((readSize = ::fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        bytes.append(buffer, readSize);
    }

    ::fclose(file);

    const std::wstring text = Utf8ToWide(bytes);

    RenameTemplate current = {};
    bool           hasCurrent = false;

    size_t position = 0;

    while (position <= text.size())
    {
        const size_t lineEnd = text.find(L'\n', position);
        std::wstring line = (lineEnd == std::wstring::npos)
                                ? text.substr(position)
                                : text.substr(position, lineEnd - position);

        // 去掉行尾的 \r
        if (!line.empty() && line.back() == L'\r')
        {
            line.pop_back();
        }

        if (line == L"---")
        {
            if (hasCurrent && !current.name.empty())
            {
                m_templates.push_back(current);
            }

            current    = {};
            hasCurrent = false;
        }
        else if (!line.empty())
        {
            const size_t equalPos = line.find(L'=');

            if (equalPos != std::wstring::npos)
            {
                const std::wstring key   = line.substr(0, equalPos);
                const std::wstring value = line.substr(equalPos + 1);

                if (key == L"name")
                {
                    current.name = value;
                    hasCurrent  = true;
                }
                else if (key == L"prefix")
                {
                    current.prefix = value;
                }
                else if (key == L"suffix")
                {
                    current.suffix = value;
                }
                else if (key == L"find")
                {
                    current.find = value;
                }
                else if (key == L"replace")
                {
                    current.replace = value;
                }
                else if (key == L"caseSensitive")
                {
                    current.caseSensitive = (value == L"1");
                }
                else if (key == L"regex")
                {
                    current.useRegex = (value == L"1");
                }
                else if (key == L"number")
                {
                    current.number = (value == L"1");
                }
                else if (key == L"start")
                {
                    current.numberStart = ::_wtoi(value.c_str());
                }
                else if (key == L"step")
                {
                    current.numberStep = ::_wtoi(value.c_str());
                }
                else if (key == L"digits")
                {
                    current.numberDigits = ::_wtoi(value.c_str());
                }
                else if (key == L"pos")
                {
                    current.numberPos = ::_wtoi(value.c_str());
                }
                else if (key == L"sep")
                {
                    current.numberSep = value;
                }
                else if (key == L"extmode")
                {
                    current.extMode = ::_wtoi(value.c_str());
                }
                else if (key == L"extvalue")
                {
                    current.extValue = value;
                }
                else if (key == L"casemode")
                {
                    current.caseMode = ::_wtoi(value.c_str());
                }
                else if (key == L"sortmode")
                {
                    current.sortMode = ::_wtoi(value.c_str());
                }
                else if (key == L"reverse")
                {
                    current.sortReverse = (value == L"1");
                }
            }
        }

        if (lineEnd == std::wstring::npos)
        {
            break;
        }

        position = lineEnd + 1;
    }

    if (hasCurrent && !current.name.empty())
    {
        m_templates.push_back(current);
    }

    RefreshTemplateCombo();
}

void BatchRenameDialog::SaveTemplatesToDisk() const
{
    const std::wstring path = TemplateFilePath();
    if (path.empty())
    {
        return;
    }

    std::wstring text;

    for (const auto& item : m_templates)
    {
        text += L"name=" + item.name + L"\r\n";
        text += L"prefix=" + item.prefix + L"\r\n";
        text += L"suffix=" + item.suffix + L"\r\n";
        text += L"find=" + item.find + L"\r\n";
        text += L"replace=" + item.replace + L"\r\n";
        text += std::wstring(L"caseSensitive=") + (item.caseSensitive ? L"1" : L"0") + L"\r\n";
        text += std::wstring(L"regex=") + (item.useRegex ? L"1" : L"0") + L"\r\n";
        text += std::wstring(L"number=") + (item.number ? L"1" : L"0") + L"\r\n";
        text += L"start=" + std::to_wstring(item.numberStart) + L"\r\n";
        text += L"step=" + std::to_wstring(item.numberStep) + L"\r\n";
        text += L"digits=" + std::to_wstring(item.numberDigits) + L"\r\n";
        text += L"pos=" + std::to_wstring(item.numberPos) + L"\r\n";
        text += L"sep=" + item.numberSep + L"\r\n";
        text += L"extmode=" + std::to_wstring(item.extMode) + L"\r\n";
        text += L"extvalue=" + item.extValue + L"\r\n";
        text += L"casemode=" + std::to_wstring(item.caseMode) + L"\r\n";
        text += L"sortmode=" + std::to_wstring(item.sortMode) + L"\r\n";
        text += std::wstring(L"reverse=") + (item.sortReverse ? L"1" : L"0") + L"\r\n";
        text += L"---\r\n";
    }

    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr)
    {
        Logger::Warn(L"批量重命名模板保存失败（打不开文件）");
        return;
    }

    const std::string bytes = WideToUtf8(text);
    ::fwrite(bytes.c_str(), 1, bytes.size(), file);
    ::fclose(file);
}

void BatchRenameDialog::RefreshTemplateCombo()
{
    if (m_hTemplateCombo == nullptr)
    {
        return;
    }

    // 先记住当前选中/输入的名字，刷新后尽量还原
    const std::wstring currentText = GetTextOf(m_hTemplateCombo);

    ::SendMessageW(m_hTemplateCombo, CB_RESETCONTENT, 0, 0);

    for (const auto& item : m_templates)
    {
        ::SendMessageW(m_hTemplateCombo, CB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(item.name.c_str()));
    }

    if (!currentText.empty())
    {
        for (size_t i = 0; i < m_templates.size(); ++i)
        {
            if (m_templates[i].name == currentText)
            {
                ::SendMessageW(m_hTemplateCombo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
                return;
            }
        }

        // 不在列表里（新名字）：留在输入框里，方便直接点保存
        ::SetWindowTextW(m_hTemplateCombo, currentText.c_str());
        return;
    }

    // 编辑框原本是空的、又有模板：默认选中第一个，
    // 这样窗口一打开框里就显示模板名，点「载入」马上有效果（不用先展开下拉挑）
    if (!m_templates.empty())
    {
        ::SendMessageW(m_hTemplateCombo, CB_SETCURSEL, 0, 0);
    }
}

void BatchRenameDialog::CaptureCurrentRules(RenameTemplate& out) const
{
    if (m_hTemplateCombo != nullptr)
    {
        out.name = GetTextOf(m_hTemplateCombo);
    }

    out.prefix  = GetTextOf(m_hPrefix);
    out.suffix  = GetTextOf(m_hSuffix);
    out.find    = GetTextOf(m_hFind);
    out.replace = GetTextOf(m_hReplace);

    out.caseSensitive = (m_hCaseSensitive != nullptr) &&
                        (::SendMessageW(m_hCaseSensitive, BM_GETCHECK, 0, 0) == BST_CHECKED);
    out.useRegex = (m_hRegex != nullptr) &&
                   (::SendMessageW(m_hRegex, BM_GETCHECK, 0, 0) == BST_CHECKED);

    out.number       = (m_hNumberEnable != nullptr) &&
                       (::SendMessageW(m_hNumberEnable, BM_GETCHECK, 0, 0) == BST_CHECKED);
    out.numberStart  = static_cast<int>(ReadInteger(m_hNumberStart, 1, 0));
    out.numberStep   = static_cast<int>(ReadInteger(m_hNumberStep, 1, 1));
    out.numberDigits = static_cast<int>(ReadInteger(m_hNumberDigits, 1, 1));
    out.numberPos    = ComboSelection(m_hNumberPos);
    out.numberSep    = GetTextOf(m_hNumberSep);

    out.extMode  = ComboSelection(m_hExtMode);
    out.extValue = GetTextOf(m_hExtValue);

    out.caseMode    = ComboSelection(m_hCaseMode);
    out.sortMode    = ComboSelection(m_hSortMode);
    out.sortReverse = (m_hSortReverse != nullptr) &&
                      (::SendMessageW(m_hSortReverse, BM_GETCHECK, 0, 0) == BST_CHECKED);
}

void BatchRenameDialog::ApplyTemplateToControls(const RenameTemplate& tpl)
{
    m_suppressRecalculate = true;

    if (m_hPrefix != nullptr)
    {
        ::SetWindowTextW(m_hPrefix, tpl.prefix.c_str());
    }
    if (m_hSuffix != nullptr)
    {
        ::SetWindowTextW(m_hSuffix, tpl.suffix.c_str());
    }
    if (m_hFind != nullptr)
    {
        ::SetWindowTextW(m_hFind, tpl.find.c_str());
    }
    if (m_hReplace != nullptr)
    {
        ::SetWindowTextW(m_hReplace, tpl.replace.c_str());
    }
    if (m_hCaseSensitive != nullptr)
    {
        ::SendMessageW(m_hCaseSensitive, BM_SETCHECK,
                       tpl.caseSensitive ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (m_hRegex != nullptr)
    {
        ::SendMessageW(m_hRegex, BM_SETCHECK,
                       tpl.useRegex ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (m_hNumberEnable != nullptr)
    {
        ::SendMessageW(m_hNumberEnable, BM_SETCHECK,
                       tpl.number ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (m_hNumberStart != nullptr)
    {
        ::SetWindowTextW(m_hNumberStart, std::to_wstring(tpl.numberStart).c_str());
    }
    if (m_hNumberStep != nullptr)
    {
        ::SetWindowTextW(m_hNumberStep, std::to_wstring(tpl.numberStep).c_str());
    }
    if (m_hNumberDigits != nullptr)
    {
        ::SetWindowTextW(m_hNumberDigits, std::to_wstring(tpl.numberDigits).c_str());
    }
    if (m_hNumberPos != nullptr)
    {
        ::SendMessageW(m_hNumberPos, CB_SETCURSEL, static_cast<WPARAM>(tpl.numberPos), 0);
    }
    if (m_hNumberSep != nullptr)
    {
        ::SetWindowTextW(m_hNumberSep, tpl.numberSep.c_str());
    }
    if (m_hExtMode != nullptr)
    {
        ::SendMessageW(m_hExtMode, CB_SETCURSEL, static_cast<WPARAM>(tpl.extMode), 0);
    }
    if (m_hExtValue != nullptr)
    {
        ::SetWindowTextW(m_hExtValue, tpl.extValue.c_str());
    }
    if (m_hCaseMode != nullptr)
    {
        ::SendMessageW(m_hCaseMode, CB_SETCURSEL, static_cast<WPARAM>(tpl.caseMode), 0);
    }

    // 排序：载入模板时一律回到模板里记的那个排序（同时丢掉之前的手动顺序）
    if (m_hSortMode != nullptr)
    {
        ::SendMessageW(m_hSortMode, CB_SETCURSEL, static_cast<WPARAM>(tpl.sortMode), 0);
    }
    if (m_hSortReverse != nullptr)
    {
        ::SendMessageW(m_hSortReverse, BM_SETCHECK,
                       tpl.sortReverse ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    if (tpl.sortMode != 3)
    {
        m_order.clear();   // 回到按规则排序
    }

    m_suppressRecalculate = false;
}

// ============================================================================
// 预览列表的拖动排序
// ============================================================================

void BatchRenameDialog::BeginListDrag(int itemIndex)
{
    if (m_hList == nullptr || itemIndex < 0)
    {
        return;
    }

    const int itemCount = ListView_GetItemCount(m_hList);

    // 拖的是"选中的那些行"；如果按下的那行没被选中，就只拖它自己
    m_dragRows.clear();
    bool anchorSelected = false;

    for (int i = 0; i < itemCount; ++i)
    {
        if (ListView_GetItemState(m_hList, i, LVIS_SELECTED) == LVIS_SELECTED)
        {
            m_dragRows.push_back(static_cast<size_t>(i));

            if (i == itemIndex)
            {
                anchorSelected = true;
            }
        }
    }

    if (!anchorSelected)
    {
        m_dragRows.assign(1, static_cast<size_t>(itemIndex));
    }

    // 整张表都被选中时拖动没有意义（等于原地不动），直接不算拖
    if (m_dragRows.size() >= static_cast<size_t>(itemCount))
    {
        m_dragRows.clear();
        return;
    }

    m_dragging = true;
    m_dropRow  = -1;
    m_dropAfter = false;

    ::SetCapture(m_hWnd);
}

void BatchRenameDialog::UpdateListDrag(int clientY)
{
    if (!m_dragging || m_hList == nullptr)
    {
        return;
    }

    const int itemCount = ListView_GetItemCount(m_hList);

    m_dropRow   = itemCount;   // 默认放到最后
    m_dropAfter = false;

    for (int i = 0; i < itemCount; ++i)
    {
        RECT itemRect = {};
        if (!ListView_GetItemRect(m_hList, i, &itemRect, LVIR_BOUNDS))
        {
            continue;
        }

        if (clientY < itemRect.bottom)
        {
            m_dropRow   = i;
            m_dropAfter = (clientY > (itemRect.top + itemRect.bottom) / 2);
            break;
        }
    }

    // 插入标记：注意 LVM_SETINSERTMARK 收的是 LVINSERTMARK 结构指针，
    // 不是 MAKELPARAM 打包的两个整数（把打包值当指针用会直接崩）
    LVINSERTMARK mark = {};
    mark.cbSize  = sizeof(LVINSERTMARK);
    mark.dwFlags = m_dropAfter ? LVIM_AFTER : 0;
    mark.iItem   = m_dropRow;

    ::SendMessageW(m_hList, LVM_SETINSERTMARK, 0, reinterpret_cast<LPARAM>(&mark));
}

void BatchRenameDialog::FinishListDrag()
{
    if (!m_dragging)
    {
        return;
    }

    m_dragging = false;

    if (::GetCapture() == m_hWnd)
    {
        ::ReleaseCapture();
    }

    // 收掉插入标记（iItem = -1 就是清除）
    if (m_hList != nullptr)
    {
        LVINSERTMARK mark = {};
        mark.cbSize  = sizeof(LVINSERTMARK);
        mark.dwFlags = 0;
        mark.iItem   = -1;
        ::SendMessageW(m_hList, LVM_SETINSERTMARK, 0, reinterpret_cast<LPARAM>(&mark));
    }

    if (m_dropRow < 0 || m_hList == nullptr)
    {
        return;
    }

    const int itemCount = static_cast<int>(m_rows.size());

    // 算出"移除被拖行之后"，插入点是第几个
    size_t removedBefore = 0;
    for (size_t row : m_dragRows)
    {
        if (static_cast<int>(row) < m_dropRow)
        {
            ++removedBefore;
        }
    }

    size_t insertAt = static_cast<size_t>(m_dropRow) - removedBefore + (m_dropAfter ? 1 : 0);

    std::vector<size_t> newOrder;   // 元素是 m_items 的下标
    newOrder.reserve(static_cast<size_t>(itemCount));

    for (int i = 0; i < itemCount; ++i)
    {
        const bool isDragged =
            std::find(m_dragRows.begin(), m_dragRows.end(), static_cast<size_t>(i)) != m_dragRows.end();

        if (!isDragged)
        {
            if (newOrder.size() == insertAt)
            {
                for (size_t row : m_dragRows)
                {
                    newOrder.push_back(m_rows[row].itemIndex);
                }
            }

            newOrder.push_back(m_rows[static_cast<size_t>(i)].itemIndex);
        }
    }

    // 拖到最末尾时上面那个循环不会再给机会插，收个尾
    if (newOrder.size() < static_cast<size_t>(itemCount))
    {
        for (size_t row : m_dragRows)
        {
            newOrder.push_back(m_rows[row].itemIndex);
        }
    }

    m_dragRows.clear();
    m_dropRow = -1;

    if (newOrder.size() != static_cast<size_t>(itemCount))
    {
        return;
    }

    m_order = newOrder;

    // 手动排过就切到"自定义顺序"，否则下一次重算又按文件名排回去，用户白拖
    if (m_hSortMode != nullptr)
    {
        ::SendMessageW(m_hSortMode, CB_SETCURSEL, 3, 0);
    }

    Recalculate();
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

    // 1) 查找替换（普通文字，或按正则处理）
    const std::wstring find = GetTextOf(m_hFind);
    if (!find.empty())
    {
        const bool useRegex = (m_hRegex != nullptr) &&
                              (::SendMessageW(m_hRegex, BM_GETCHECK, 0, 0) == BST_CHECKED);

        const std::wstring replacement = GetTextOf(m_hReplace);

        if (useRegex)
        {
            // 正则的合法性在外面已经编译验证过（m_regexValid），这里直接用缓存的那份
            if (m_regexValid && EnsureRegexCompiled(find))
            {
                try
                {
                    stem = std::regex_replace(stem, g_regexCache.regex, replacement);
                }
                catch (const std::regex_error&)
                {
                    // 替换串里写了非法反向引用之类：保持原名，状态列那边会标出来
                }
            }
        }
        else
        {
            const bool caseSensitive =
                (m_hCaseSensitive != nullptr) &&
                (::SendMessageW(m_hCaseSensitive, BM_GETCHECK, 0, 0) == BST_CHECKED);

            stem = ReplaceAll(stem, find, replacement, caseSensitive);
        }
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

    // ---- 正则先验证一次：写法有误就整张表标红，别让用户以为能改 ----
    const std::wstring findText = GetTextOf(m_hFind);
    const bool useRegex = (m_hRegex != nullptr) &&
                          (::SendMessageW(m_hRegex, BM_GETCHECK, 0, 0) == BST_CHECKED);

    m_regexValid = true;

    if (useRegex && !findText.empty())
    {
        m_regexValid = EnsureRegexCompiled(findText);
    }

    // ---- 排出"谁先谁后"，序号就按这个次序给 ----
    const int  sortMode = ComboSelection(m_hSortMode);
    const bool reverseOn = (m_hSortReverse != nullptr) &&
                           (::SendMessageW(m_hSortReverse, BM_GETCHECK, 0, 0) == BST_CHECKED);

    std::vector<size_t> order;

    if (sortMode == 3 && m_order.size() == m_items.size())
    {
        // 自定义顺序：就是用户在表里拖出来的那个次序（倒序开关对它不起作用）
        order = m_order;
    }
    else
    {
        order.resize(m_items.size());
        for (size_t i = 0; i < order.size(); ++i)
        {
            order[i] = i;
        }

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

        // 记住这份顺序：以后切成"自定义顺序"或者拖动调整都从它出发
        m_order = order;
    }

    // ---- 正则写法有误：整表标红，一个都不许应用 ----
    if (useRegex && !findText.empty() && !m_regexValid)
    {
        for (size_t i = 0; i < order.size(); ++i)
        {
            PreviewRow row = {};
            row.itemIndex = order[i];
            row.oldName   = m_items[order[i]].name;
            row.newName   = m_items[order[i]].name;
            row.state     = 2;
            row.stateText = L"正则写法有误";
            m_rows.push_back(row);
        }
        return;
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

    const int existingCount = static_cast<int>(ListView_GetItemCount(m_hList));

    ::SendMessageW(m_hList, WM_SETREDRAW, FALSE, 0);

    // 行数没变就只改文字：不整表重建，输入的时候才不会一行一行地闪
    if (existingCount == static_cast<int>(m_rows.size()))
    {
        for (int i = 0; i < existingCount; ++i)
        {
            const PreviewRow& row = m_rows[static_cast<size_t>(i)];

            ListView_SetItemText(m_hList, i, 0, const_cast<wchar_t*>(row.oldName.c_str()));
            ListView_SetItemText(m_hList, i, 1, const_cast<wchar_t*>(row.newName.c_str()));
            ListView_SetItemText(m_hList, i, 2, const_cast<wchar_t*>(row.stateText.c_str()));
        }
    }
    else
    {
        ListView_DeleteAllItems(m_hList);

        for (int i = 0; i < static_cast<int>(m_rows.size()); ++i)
        {
            const PreviewRow& row = m_rows[static_cast<size_t>(i)];

            LVITEMW item = {};
            item.mask    = LVIF_TEXT;
            item.iItem   = i;
            item.pszText = const_cast<wchar_t*>(row.oldName.c_str());
            ListView_InsertItem(m_hList, &item);

            ListView_SetItemText(m_hList, i, 1, const_cast<wchar_t*>(row.newName.c_str()));
            ListView_SetItemText(m_hList, i, 2, const_cast<wchar_t*>(row.stateText.c_str()));
        }
    }

    ::SendMessageW(m_hList, WM_SETREDRAW, TRUE, 0);

    // 只重画列表自己的可见区域（不擦背景，避免白闪）
    ::InvalidateRect(m_hList, nullptr, FALSE);
    ::UpdateWindow(m_hList);
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

    // 只重画底部那一行统计文字：之前整窗 InvalidateRect，
    // 父窗口会先把整块刷白再等子控件重画，输入一个字就闪一下。
    if (m_hWnd != nullptr)
    {
        RECT statsRect = m_statsRect;

        // 冲突提示那一行在统计文字上面，一起带上
        statsRect.top -= Scale(22);

        ::InvalidateRect(m_hWnd, &statsRect, FALSE);
        ::UpdateWindow(m_hWnd);
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

    // ---- 三条分隔线（列表上方 / 列表下方 / 模板栏下方）----
    HPEN dividerPen = ::CreatePen(PS_SOLID, 1, kDividerColor);
    HGDIOBJ oldPen = ::SelectObject(dc, dividerPen);

    ::MoveToEx(dc, padding, Scale(kListTop - 6), nullptr);
    ::LineTo(dc, clientRect.right - padding, Scale(kListTop - 6));

    ::MoveToEx(dc, padding, Scale(kTemplateTop - 12), nullptr);
    ::LineTo(dc, clientRect.right - padding, Scale(kTemplateTop - 12));

    ::MoveToEx(dc, padding, Scale(kRuleTop - 12), nullptr);
    ::LineTo(dc, clientRect.right - padding, Scale(kRuleTop - 12));

    ::SelectObject(dc, oldPen);
    ::DeleteObject(dividerPen);

    // ---- 模板栏：标签 + 三个按钮 ----
    ::SelectObject(dc, m_font);
    ::SetTextColor(dc, kTitleColor);

    {
        RECT labelRect = { padding, Scale(kTemplateTop) - Scale(2),
                           padding + Scale(44), Scale(kTemplateTop) + Scale(28) };
        ::DrawTextW(dc, L"模板", -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    DrawButton(dc, m_templateLoadRect,   L"载入", m_hotTemplateLoad,   m_pressedTemplateLoad,   false);
    DrawButton(dc, m_templateSaveRect,   L"保存", m_hotTemplateSave,   m_pressedTemplateSave,   false);
    DrawButton(dc, m_templateDeleteRect, L"删除", m_hotTemplateDelete, m_pressedTemplateDelete, false);

    // 模板栏右边一行小字，说明怎么用（不影响规则区）
    if (m_templates.empty())
    {
        ::SelectObject(dc, m_fontSmall);
        ::SetTextColor(dc, kHintColor);

        RECT tipRect = { m_templateDeleteRect.right + Scale(16), Scale(kTemplateTop) - Scale(2),
                         clientRect.right - padding, Scale(kTemplateTop) + Scale(28) };
        ::DrawTextW(dc, L"先把规则摆好，起个名字，点「保存」。",
                    -1, &tipRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        ::SelectObject(dc, m_font);
    }
    else
    {
        ::SelectObject(dc, m_fontSmall);
        ::SetTextColor(dc, kHintColor);

        RECT tipRect = { m_templateDeleteRect.right + Scale(16), Scale(kTemplateTop) - Scale(2),
                         clientRect.right - padding, Scale(kTemplateTop) + Scale(28) };
        ::DrawTextW(dc, L"选一个名字点「载入」；换个新名字点「保存」就是新建。",
                    -1, &tipRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        ::SelectObject(dc, m_font);
    }

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
    ::DrawTextW(dc, statsText, -1, &m_statsRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 有冲突时「应用」不可用，明说原因（这一行在统计文字上面）
    if (conflictCount > 0)
    {
        RECT warnRect = m_statsRect;
        warnRect.bottom = m_statsRect.top;
        warnRect.top    = m_statsRect.top - Scale(20);

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
            // 下拉列表正展开的时候，回车的意思是"收下拉"，不能顺手把窗口应用掉
            // （否则用户在「常用正则」里按回车选条目，窗口就被关掉了）
            const HWND combos[] =
            {
                m_hRegexPreset, m_hTemplateCombo, m_hNumberPos,
                m_hExtMode, m_hCaseMode, m_hSortMode
            };

            for (HWND combo : combos)
            {
                if (combo != nullptr &&
                    ::SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) != 0)
                {
                    return 0;
                }
            }

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

            // ---- 模板三个按钮 ----
            if (controlId == kIdTemplateLoad)
            {
                const std::wstring name = GetTextOf(m_hTemplateCombo);

                for (const auto& item : m_templates)
                {
                    if (item.name == name)
                    {
                        ApplyTemplateToControls(item);
                        Recalculate();
                        break;
                    }
                }
                return 0;
            }

            if (controlId == kIdTemplateSave)
            {
                RenameTemplate captured = {};
                CaptureCurrentRules(captured);

                if (captured.name.empty())
                {
                    // 没写名字：拿"模板 N"顶一下，省得用户卡住
                    captured.name = L"模板 " + std::to_wstring(m_templates.size() + 1);
                }

                // 同名覆盖，否则追加
                bool replaced = false;
                for (auto& item : m_templates)
                {
                    if (item.name == captured.name)
                    {
                        item       = captured;
                        replaced   = true;
                        break;
                    }
                }

                if (!replaced)
                {
                    m_templates.push_back(captured);
                }

                SaveTemplatesToDisk();
                RefreshTemplateCombo();
                ::SetWindowTextW(m_hTemplateCombo, captured.name.c_str());
                ::InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }

            if (controlId == kIdTemplateDelete)
            {
                const std::wstring name = GetTextOf(m_hTemplateCombo);

                for (size_t i = 0; i < m_templates.size(); ++i)
                {
                    if (m_templates[i].name == name)
                    {
                        // 删除是不可逆的，先问一句
                        const int answer =
                            ::MessageBoxW(hWnd, (L"要删除模板「" + name + L"」吗？").c_str(),
                                          L"批量重命名", MB_YESNO | MB_ICONQUESTION);

                        if (answer != IDYES)
                        {
                            return 0;
                        }

                        m_templates.erase(m_templates.begin() + static_cast<ptrdiff_t>(i));
                        SaveTemplatesToDisk();
                        RefreshTemplateCombo();
                        ::InvalidateRect(hWnd, nullptr, FALSE);
                        break;
                    }
                }
                return 0;
            }
        }

        // 模板下拉里挑一个名字 → 直接套用（"单击模板一键载入"，不用再点按钮）
        if (controlId == kIdTemplateCombo && notification == CBN_SELCHANGE)
        {
            const std::wstring name = GetTextOf(m_hTemplateCombo);

            for (const auto& item : m_templates)
            {
                if (item.name == name)
                {
                    ApplyTemplateToControls(item);
                    Recalculate();
                    break;
                }
            }
            return 0;
        }

        // 常用正则下拉：选中一条 → 填进「查找」并勾上正则
        if (controlId == kIdRegexPreset && notification == CBN_SELCHANGE)
        {
            const size_t presetIndex = static_cast<size_t>(ComboSelection(m_hRegexPreset));

            if (presetIndex > 0)
            {
                m_suppressRecalculate = true;

                if (m_hFind != nullptr)
                {
                    ::SetWindowTextW(m_hFind, RegexPresetPattern(presetIndex));
                }
                if (m_hRegex != nullptr)
                {
                    ::SendMessageW(m_hRegex, BM_SETCHECK, BST_CHECKED, 0);
                }

                m_suppressRecalculate = false;
                Recalculate();
            }
            return 0;
        }

        // 规则控件的任何改动 → 重算预览
        if (controlId >= kIdPrefix && controlId <= kIdRegex)
        {
            if (notification == EN_CHANGE || notification == CBN_SELCHANGE ||
                notification == BN_CLICKED)
            {
                if (!m_suppressRecalculate)
                {
                    Recalculate();
                }
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
            header->code == LVN_BEGINDRAG)
        {
            const NMLISTVIEW* listView = reinterpret_cast<NMLISTVIEW*>(lParam);
            BeginListDrag(listView->iItem);
            return 0;
        }

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

        // 正在拖列表：把主窗口坐标换成列表坐标，算出插入位置
        if (m_dragging)
        {
            POINT point = { x, y };
            ::MapWindowPoints(hWnd, m_hList, &point, 1);
            UpdateListDrag(point.y);
            return 0;
        }

        const bool overCancel = (x >= m_cancelRect.left && x < m_cancelRect.right &&
                                 y >= m_cancelRect.top  && y < m_cancelRect.bottom);
        const bool overApply  = (x >= m_applyRect.left && x < m_applyRect.right &&
                                 y >= m_applyRect.top  && y < m_applyRect.bottom);

        const bool overTplLoad = (x >= m_templateLoadRect.left && x < m_templateLoadRect.right &&
                                  y >= m_templateLoadRect.top  && y < m_templateLoadRect.bottom);
        const bool overTplSave = (x >= m_templateSaveRect.left && x < m_templateSaveRect.right &&
                                  y >= m_templateSaveRect.top  && y < m_templateSaveRect.bottom);
        const bool overTplDel  = (x >= m_templateDeleteRect.left && x < m_templateDeleteRect.right &&
                                  y >= m_templateDeleteRect.top  && y < m_templateDeleteRect.bottom);

        if (overCancel != m_hotCancel || overApply != m_hotApply ||
            overTplLoad != m_hotTemplateLoad || overTplSave != m_hotTemplateSave ||
            overTplDel != m_hotTemplateDelete)
        {
            m_hotCancel = overCancel;
            m_hotApply  = overApply;
            m_hotTemplateLoad   = overTplLoad;
            m_hotTemplateSave   = overTplSave;
            m_hotTemplateDelete = overTplDel;
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
        if (m_hotCancel || m_hotApply || m_hotTemplateLoad ||
            m_hotTemplateSave || m_hotTemplateDelete)
        {
            ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
        }

        return 0;
    }

    case WM_MOUSELEAVE:
        m_trackingLeave = false;

        if (m_hotCancel || m_hotApply || m_hotTemplateLoad ||
            m_hotTemplateSave || m_hotTemplateDelete)
        {
            m_hotCancel = false;
            m_hotApply  = false;
            m_hotTemplateLoad   = false;
            m_hotTemplateSave   = false;
            m_hotTemplateDelete = false;
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

        const bool overTplLoad = (x >= m_templateLoadRect.left && x < m_templateLoadRect.right &&
                                  y >= m_templateLoadRect.top  && y < m_templateLoadRect.bottom);
        const bool overTplSave = (x >= m_templateSaveRect.left && x < m_templateSaveRect.right &&
                                  y >= m_templateSaveRect.top  && y < m_templateSaveRect.bottom);
        const bool overTplDel  = (x >= m_templateDeleteRect.left && x < m_templateDeleteRect.right &&
                                  y >= m_templateDeleteRect.top  && y < m_templateDeleteRect.bottom);

        m_pressedCancel = overCancel;
        m_pressedApply  = overApply;
        m_pressedTemplateLoad   = overTplLoad;
        m_pressedTemplateSave   = overTplSave;
        m_pressedTemplateDelete = overTplDel;

        if (overCancel || overApply || overTplLoad || overTplSave || overTplDel)
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

        // 拖动排序收尾（这时候鼠标可能在列表里也可能拖到外面）
        if (m_dragging)
        {
            FinishListDrag();
            return 0;
        }

        if (::GetCapture() == hWnd)
        {
            ::ReleaseCapture();
        }

        const bool overCancel = (x >= m_cancelRect.left && x < m_cancelRect.right &&
                                 y >= m_cancelRect.top  && y < m_cancelRect.bottom);
        const bool overApply  = (x >= m_applyRect.left && x < m_applyRect.right &&
                                 y >= m_applyRect.top  && y < m_applyRect.bottom);

        const bool overTplLoad = (x >= m_templateLoadRect.left && x < m_templateLoadRect.right &&
                                  y >= m_templateLoadRect.top  && y < m_templateLoadRect.bottom);
        const bool overTplSave = (x >= m_templateSaveRect.left && x < m_templateSaveRect.right &&
                                  y >= m_templateSaveRect.top  && y < m_templateSaveRect.bottom);
        const bool overTplDel  = (x >= m_templateDeleteRect.left && x < m_templateDeleteRect.right &&
                                  y >= m_templateDeleteRect.top  && y < m_templateDeleteRect.bottom);

        const bool wasCancel = m_pressedCancel;
        const bool wasApply  = m_pressedApply;
        const bool wasTplLoad = m_pressedTemplateLoad;
        const bool wasTplSave = m_pressedTemplateSave;
        const bool wasTplDel  = m_pressedTemplateDelete;

        m_pressedCancel = false;
        m_pressedApply  = false;
        m_pressedTemplateLoad   = false;
        m_pressedTemplateSave   = false;
        m_pressedTemplateDelete = false;
        ::InvalidateRect(hWnd, nullptr, FALSE);

        if (wasCancel && overCancel)
        {
            ::SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(kIdCancel, BN_CLICKED), 0);
        }
        else if (wasApply && overApply)
        {
            ::SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(kIdApply, BN_CLICKED), 0);
        }
        else if (wasTplLoad && overTplLoad)
        {
            ::SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(kIdTemplateLoad, BN_CLICKED), 0);
        }
        else if (wasTplSave && overTplSave)
        {
            ::SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(kIdTemplateSave, BN_CLICKED), 0);
        }
        else if (wasTplDel && overTplDel)
        {
            ::SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(kIdTemplateDelete, BN_CLICKED), 0);
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
