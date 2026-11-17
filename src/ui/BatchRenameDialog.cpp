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
#include <uxtheme.h>   // 主题绘制（复选框等控件的原生外观）

#include <algorithm>
#include <string.h>
#include <cwctype>
#include <regex>
#include <stdio.h>
#include <stdlib.h>

#include "ui/BatchRenameDialog.h"

#include "core/Logger.h"

#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comctl32.lib")   // TaskDialog 用它（模板未保存时那个三选提醒）

namespace
{

const wchar_t* kClassName = L"FileExplorerBatchRename";

// 客户区大小（96 DPI 下的逻辑尺寸）
const int kClientWidth  = 940;
const int kClientHeight = 642;   // 参数区从 4 行减到 3 行，窗口跟着矮一行（34px）
const int kPadding      = 20;

// 预览列表的位置（高度从 246 加到 293，多出来的 47px 正好吃掉规则区和底部按钮之间那块空白）
const int kListTop    = 78;
const int kListHeight = 293;

// 模板栏（预览列表和规则区之间那一条）：固定 10 个槽，一行排满
const int kTemplateTop     = 389;
const int kTemplateSlotGap = 4;    // 槽间距（原来是 8，收一半；槽宽在 Layout 里按表格宽度均分算）
const int kTemplateSlotH   = 30;   // 槽高

// 底部一排按钮（取消 / 清空 / 保存 / 应用）
const int kBottomButtonW   = 90;
const int kBottomButtonGap = 12;

// 参数区：三行，每行一个 1px 细边框"分组框"，行内控件统一高度；
// kRuleRowHeight 是行与行的间距，kRuleBandHeight 是分组框高度
const int kRuleTop          = 445;
const int kRuleRowHeight    = 48;   // 行距（比分组框高，多出来的部分就是行与行之间的白缝）
const int kRuleBandHeight   = 36;   // 每行分组框的高度
const int kRuleBandInset    = 0;    // 分组框跟表格同宽（都从左 padding 到右 padding，不再外扩）
const int kRuleControlHeight = 24;  // 行内所有控件（输入框 / 下拉 / 复选框 / 按钮）统一高度
const int kRuleControlTop    = 6;   // 控件相对分组条顶部的偏移：(36-24)/2
const int kRuleRightX        = 920; // 三行内容的右边界，统一收在这一列
const int kRuleSortDividerX  = 736; // 第三行里"序号"和"排序"之间的竖分隔线位置（两边各留 12px）

// 底部那条线：现在只剩统计文字 + 四个按钮（排序 / 倒序已经搬到参数区第三行）

// ----------------------------------------------------------------------------
// 参数区（3 行）里每个字段的位置
//
// Layout 摆控件、Draw 画标签，两边都从这一份表里取坐标：
// 以前是 Layout 写一遍、Draw 再写一遍，改一处忘一处，标签就跟输入框错位了。
// 数值都是 100% 缩放下的逻辑像素，Layout 里统一 Scale 一遍。
// label 为 nullptr 表示这一格只有控件、没有左边的文字标签。
// ----------------------------------------------------------------------------
enum RuleField
{
    kRfPrefix = 0,      // 前缀
    kRfSuffix,          // 后缀
    kRfExtMode,         // 扩展名（下拉：不修改 / 修改）
    kRfExtValue,        // 扩展名填什么（选「修改」时才启用，空 = 去掉扩展名）
    kRfCaseMode,        // 大小写（下拉）

    kRfFind,            // 查找
    kRfRegexButton,     // 查找方式（自绘按钮：显示「普通」或「正则」，点开弹菜单）
    kRfReplace,         // 替换为
    kRfIncludeExt,      // 含扩展名（复选框）
    kRfCaseSensitive,   // 区分大小写（复选框）

    kRfNumberEnable,    // 加序号（复选框）
    kRfNumberReplace,   // 替换（复选框：序号直接当新名字，原文件名不要了）
    kRfNumberStart,     // 起始
    kRfNumberStep,      // 步长
    kRfNumberDigits,    // 位数
    kRfNumberPos,       // 位置（下拉）
    kRfNumberSep,       // 分隔符

    kRfSortMode,        // 排序依据（下拉）
    kRfSortReverse,     // 倒序（复选框）

    kRfCount
};

struct RuleFieldSpec
{
    const wchar_t* label;      // 标签文字，nullptr = 没有标签
    int            labelX;     // 标签左边缘
    int            labelWidth; // 标签宽
    int            x;          // 控件左边缘
    int            width;      // 控件宽
    int            row;        // 第几行（0 起）
    int            height;     // 控件高（下拉要给展开后的高度）
};

// 参数区三行：三行各自用一个 1px 细边框"框成一组"（白底只描边，不填色），
// 行距 48、框高 36，行与行之间留 12px 白缝。
// 网格：框是表格那一列的宽度（20~920），框里左右各留 8px 内边距（内容 28~912），
// 这样框线不会被控件盖住；标签按字数给紧（2 字 30 / 3 字 44），标签右边缘到控件固定 5px。
const RuleFieldSpec kRuleSpecs[kRfCount] =
{
    // ---- 第 0 行：前缀 / 后缀 / 扩展名（下拉 + 值）/ 大小写 ----
    // 两个下拉（不修改 / 保持原样）只留够显示字的宽度，省下来的 50px 全给了前后缀两个输入框
    { L"前缀",    28, 30,  63, 245, 0, 24 },         // 前缀
    { L"后缀",   316, 30, 351, 245, 0, 24 },         // 后缀
    { L"扩展名", 604, 44, 653,  72, 0, 24 * 3 },     // 扩展名下拉（宽度 72 够显示「不修改」）
    { nullptr,   -1, -1, 725,  50, 0, 25 },          // 扩展名填什么（紧贴下拉，向下加高 1px）
    { L"大小写", 783, 44, 832,  80, 0, 24 * 3 },     // 大小写下拉（宽度 80 够显示「保持原样」）

    // ---- 第 1 行：查找（紧贴「普通 / 正则」按钮）/ 替换为（跟查找一样长）/ 两个复选框 ----
    { L"查找",    28, 30,  63, 285, 1, 24 },         // 查找
    { nullptr,   -1, -1, 348,  36, 1, 24 },          // 查找方式按钮（紧贴输入框，宽度 36）
    { L"替换为", 392, 44, 441, 285, 1, 24 },         // 替换为（跟查找等长）
    { nullptr,   -1, -1, 734,  80, 1, 24 },          // 含扩展名
    { nullptr,   -1, -1, 822,  90, 1, 24 },          // 区分大小写（右端留 8px 到框线）

    // ---- 第 2 行：加序号 / 替换 / 三个数字 / 位置 / 分隔符 ｜ 排序 / 倒序 ----
    { nullptr,   -1, -1,  28,  64, 2, 24 },          // 加序号（左端留 8px 到框线）
    { nullptr,   -1, -1, 100,  53, 2, 24 },          // 替换（勾上就只剩序号，原名不要了）
    { L"起始",  161, 30, 196,  54, 2, 24 },          // 起始
    { L"步长",  258, 30, 293,  54, 2, 24 },          // 步长
    { L"位数",  355, 30, 390,  54, 2, 24 },          // 位数
    { L"位置",  452, 30, 487, 120, 2, 24 * 3 },      // 位置下拉（2 项）
    { L"分隔符", 615, 44, 664,  60, 2, 24 },         // 分隔符（60 宽，一般就填一个符号）
    { nullptr,   -1, -1, 748,  96, 2, 24 * 5 },      // 排序下拉（4 项，跟左边序号组分得更开）
    { nullptr,   -1, -1, 852,  60, 2, 24 }           // 倒序（右端留 8px 到框线）
};

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

    kIdExtMode       = 5012,   // 扩展名：不修改 / 修改
    kIdExtValue      = 5013,   // 扩展名填什么（选「修改」时启用）
    kIdCaseMode      = 5014,   // 大小写
    kIdSortMode      = 5015,   // 排序依据（底部一行）
    kIdSortReverse   = 5016,   // 倒序（底部一行）

    kIdRegex         = 5017,   // 预留（原来的"查找按正则处理"复选框，已并入查找方式）
    kIdRegexPreset   = 5018,   // 预留（查找方式从下拉改成自绘按钮 + 菜单，不再有窗口控件）
    kIdIncludeExt    = 5019,   // 含扩展名（查找替换是否连扩展名一起处理）
    kIdNumberReplace = 5020,   // 替换（序号直接当新名字，原文件名不要了）

    kIdList          = 5100,   // 预览列表
    kIdCancel        = 5101,   // 取消（自绘按钮）
    kIdApply         = 5102,   // 应用（自绘按钮）
    kIdClear         = 5103,   // 清空（自绘按钮）
    kIdSave          = 5104,   // 保存（自绘按钮，只在选中模板时出现）
    kIdRenameEdit    = 5105    // 模板右键就地改名用的输入框
};

// 颜色（和「设置」窗口一套）
const COLORREF kBackground      = RGB(255, 255, 255);
const COLORREF kTitleColor      = RGB(32, 32, 32);
const COLORREF kHintColor       = RGB(96, 103, 111);
const COLORREF kDividerColor    = RGB(232, 235, 239);
const COLORREF kRuleBandBorder  = RGB(226, 230, 236);   // 参数区三行分组框的细边线（不填色，只描边）
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

// ----------------------------------------------------------------------------
// 只把一小块地方标成"待重画"。
// 为什么不用 InvalidateRect(hWnd, nullptr)：那样整窗都要重画，
// 列表、输入框、下拉跟着白一下，用户看到的就是"鼠标一滑满屏闪"。
// ----------------------------------------------------------------------------
inline void InvalidatePiece(HWND hWnd, const RECT& rect)
{
    if (hWnd != nullptr && rect.right > rect.left && rect.bottom > rect.top)
    {
        ::InvalidateRect(hWnd, &rect, FALSE);
    }
}

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
    if (m_disabledBrush != nullptr)
    {
        ::DeleteObject(m_disabledBrush);
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
    if (m_disabledBrush == nullptr)
    {
        // 禁用的输入框用它做底色（浅灰，跟系统"不能编辑"的样子一致）
        m_disabledBrush = ::CreateSolidBrush(RGB(240, 240, 240));
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
                               // WS_CLIPCHILDREN 很关键：不然父窗口一重绘就连列表、
                               // 输入框、下拉那些子控件的位置也一起刷白，看着就是整块闪
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
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

    m_hPrefix  = createEdit(kIdPrefix,  L"名字前面加什么");
    m_hSuffix  = createEdit(kIdSuffix,  L"名字后面加什么");
    m_hFind    = createEdit(kIdFind,    L"要查找的文字");
    m_hReplace = createEdit(kIdReplace, L"替换成什么（空＝删除）");

    m_hCaseSensitive = ::CreateWindowExW(0, L"BUTTON", L"区分大小写",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                         0, 0, 0, 0, m_hWnd,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCaseSensitive)),
                                         instance, nullptr);
    if (m_hCaseSensitive != nullptr)
    {
        ::SendMessageW(m_hCaseSensitive, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
    }

    // 「含扩展名」：勾上以后查找替换连扩展名一起处理（默认不勾，只动主文件名）
    m_hIncludeExt = ::CreateWindowExW(0, L"BUTTON", L"含扩展名",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                      0, 0, 0, 0, m_hWnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdIncludeExt)),
                                      instance, nullptr);
    if (m_hIncludeExt != nullptr)
    {
        ::SendMessageW(m_hIncludeExt, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
    }

    // 查找方式不再是下拉框，改成「普通 / 正则」自绘按钮 + 弹出菜单（见 OnRegexButtonClick）

    // ---- 模板栏：10 个槽全是自绘按钮，不需要子窗口（右键改名的输入框按需临时创建）----

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

    // 分隔符：默认就填一个「-」，跟结构体里的默认值一致；
    // 清空它才是"序号和原名直接粘在一起"（提示文字写明这一点，免得看到框是空的又以为有 -
    m_hNumberSep = createEdit(kIdNumberSep, L"空＝直接拼");
    if (m_hNumberSep != nullptr)
    {
        ::SetWindowTextW(m_hNumberSep, L"-");
    }

    // 替换：勾上以后，原来那一长串文件名整个不要了，新名字就是序号本身
    m_hNumberReplace = ::CreateWindowExW(0, L"BUTTON", L"替换",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                         0, 0, 0, 0, m_hWnd,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdNumberReplace)),
                                         instance, nullptr);
    if (m_hNumberReplace != nullptr)
    {
        ::SendMessageW(m_hNumberReplace, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
    }

    // ---- 扩展名：只有「不修改 / 修改」两项；选「修改」时右边那个框才启用 ----
    m_hExtMode = ::CreateWindowExW(0, L"COMBOBOX", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                   CBS_DROPDOWNLIST | WS_VSCROLL,
                                   0, 0, 0, 0, m_hWnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdExtMode)),
                                   instance, nullptr);
    if (m_hExtMode != nullptr)
    {
        ::SendMessageW(m_hExtMode, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        ::SendMessageW(m_hExtMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"不修改"));
        ::SendMessageW(m_hExtMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"修改"));
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

        // 列宽按真实内宽重算：原名 42% / 新名 42% / 状态 16%。
        // 这里要扣掉竖向滚动条那一条的宽度：行数多了冒出竖滚动条时客户区会变窄，
        // 三列加起来如果正好等于原宽度，就会再冒出一条横向滚动条（用户说不要横向滚动条）。
        const int innerWidth = listWidth - ::GetSystemMetrics(SM_CXVSCROLL) - Scale(2);
        ListView_SetColumnWidth(m_hList, 0, innerWidth * 42 / 100);
        ListView_SetColumnWidth(m_hList, 1, innerWidth * 42 / 100);
        ListView_SetColumnWidth(m_hList, 2, innerWidth * 16 / 100);
    }

    // ---- 参数区 3 行：坐标全部取自 kRuleSpecs，和 Draw 画标签用的是同一份表 ----
    {
        // 控件句柄按 RuleField 的顺序排好，摆位置时只按下标取
        HWND controls[kRfCount] = {};
        controls[kRfPrefix]       = m_hPrefix;
        controls[kRfSuffix]       = m_hSuffix;
        controls[kRfExtMode]      = m_hExtMode;
        controls[kRfExtValue]     = m_hExtValue;
        controls[kRfCaseMode]     = m_hCaseMode;
        controls[kRfFind]         = m_hFind;
        controls[kRfReplace]      = m_hReplace;
        controls[kRfIncludeExt]   = m_hIncludeExt;
        controls[kRfCaseSensitive] = m_hCaseSensitive;
        controls[kRfNumberEnable] = m_hNumberEnable;
        controls[kRfNumberReplace] = m_hNumberReplace;
        controls[kRfNumberStart]  = m_hNumberStart;
        controls[kRfNumberStep]   = m_hNumberStep;
        controls[kRfNumberDigits] = m_hNumberDigits;
        controls[kRfNumberPos]    = m_hNumberPos;
        controls[kRfNumberSep]    = m_hNumberSep;
        controls[kRfSortMode]     = m_hSortMode;
        controls[kRfSortReverse]  = m_hSortReverse;

        for (int i = 0; i < kRfCount; ++i)
        {
            const RuleFieldSpec& spec = kRuleSpecs[i];
            const int top = Scale(kRuleTop) + Scale(kRuleRowHeight) * spec.row;
            const int y   = top + Scale(kRuleControlTop);   // 行内控件统一从分组条顶 + 6 开始

            // 「查找方式」没有窗口控件，是一块自绘按钮，这里只记下矩形
            if (i == kRfRegexButton)
            {
                m_regexButtonRect.left   = Scale(spec.x);
                m_regexButtonRect.right  = Scale(spec.x) + Scale(spec.width);
                m_regexButtonRect.top    = y;
                m_regexButtonRect.bottom = y + Scale(kRuleControlHeight);
                continue;
            }

            HWND control = controls[i];
            if (control == nullptr)
            {
                continue;
            }

            ::MoveWindow(control, Scale(spec.x), y,
                         Scale(spec.width), Scale(spec.height), TRUE);
        }
    }

    // ---- 模板栏：10 个槽一行排满（全是自绘按钮）----
    {
        // 竖着居中在上下两条分隔线中间（上线在 kTemplateTop-12、下线在 kRuleTop-12），
        // 以前固定往上抬 2px，看着偏上；现在按两条线的中点算。
        const int lineTop    = Scale(kTemplateTop - 12);
        const int lineBottom = Scale(kRuleTop - 12);
        const int slotHeight = Scale(kTemplateSlotH);
        const int top        = lineTop + (lineBottom - lineTop - slotHeight) / 2;

        // 这一行跟表格（预览列表）同宽：从 padding 起步、到 clientRight-padding 收尾。
        // 十个槽平均分这段宽度：每格 cell = 总宽/10，槽宽 = cell - 间距，
        // 最后一格补满到右边界（这样整排正好跟表格右边缘对齐，不会差几个像素）。
        const int left     = Scale(kPadding);
        const int totalW   = (clientRect.right - Scale(kPadding)) - left;
        const int count    = static_cast<int>(BatchRenameDialog::kTemplateSlotCount);
        const int cell     = (count > 0) ? (totalW / count) : totalW;
        const int slotGap  = Scale(kTemplateSlotGap);

        m_templateRects.resize(BatchRenameDialog::kTemplateSlotCount);

        for (size_t i = 0; i < m_templateRects.size(); ++i)
        {
            RECT& rect = m_templateRects[i];
            const int index = static_cast<int>(i);

            rect.left  = left + index * cell;
            rect.top   = top;
            // 最后一格一直铺到右边界，其余格右边留 slotGap 的间距
            rect.right = rect.left + ((index == count - 1) ? cell : cell - slotGap);
            rect.bottom = rect.top + slotHeight;
        }
    }

    // ---- 底部一排四个按钮：取消 / 清空 / 保存 / 应用（右对齐，应用在最右）----
    const int buttonWidth  = Scale(kBottomButtonW);
    const int buttonHeight = Scale(32);
    const int buttonGap    = Scale(kBottomButtonGap);

    m_applyRect.right  = clientRect.right - padding;
    m_applyRect.left   = m_applyRect.right - buttonWidth;
    m_applyRect.bottom = clientRect.bottom - Scale(16);
    m_applyRect.top    = m_applyRect.bottom - buttonHeight;

    m_saveRect.right   = m_applyRect.left - buttonGap;
    m_saveRect.left    = m_saveRect.right - buttonWidth;
    m_saveRect.bottom  = m_applyRect.bottom;
    m_saveRect.top     = m_applyRect.top;

    m_clearRect.right  = m_saveRect.left - buttonGap;
    m_clearRect.left   = m_clearRect.right - buttonWidth;
    m_clearRect.bottom = m_applyRect.bottom;
    m_clearRect.top    = m_applyRect.top;

    m_cancelRect.right  = m_clearRect.left - buttonGap;
    m_cancelRect.left   = m_cancelRect.right - buttonWidth;
    m_cancelRect.bottom = m_applyRect.bottom;
    m_cancelRect.top    = m_applyRect.top;

    // ---- 底部这一行左边只剩统计文字了（排序 / 倒序已经搬到参数区第三行右侧）----
    m_statsRect.left   = padding;
    m_statsRect.right  = m_cancelRect.left - Scale(12);
    m_statsRect.top    = m_applyRect.top;
    m_statsRect.bottom = m_applyRect.bottom;
}

// ============================================================================
// 规则区启用状态（序号没勾起来时，那几个框灰掉）
// ============================================================================
void BatchRenameDialog::EnableRuleControls()
{
    const bool numberOn = (m_hNumberEnable != nullptr) &&
                          (::SendMessageW(m_hNumberEnable, BM_GETCHECK, 0, 0) == BST_CHECKED);

    // 注意：「替换」这个复选框故意不放进禁用列表。
    // 系统在"禁用"状态会给复选框自己刷一块 COLOR_3DFACE(240,240,240) 的灰底，
    // 父窗口无论返回什么刷子都盖不住，白底分组框里就冒出一个灰方块。
    // 它本来也只在勾了「加序号」时才起作用，没勾时勾着它也无害，所以保持可点即可。
    for (HWND control : { m_hNumberStart, m_hNumberStep, m_hNumberDigits })
    {
        if (control != nullptr)
        {
            ::EnableWindow(control, numberOn ? TRUE : FALSE);
        }
    }

    // 「位置」「分隔符」一直保持可用（勾了「替换」也不会变灰）：
    // 替换只是把原名整段顶掉，这两项在替换态暂时不参与计算，取消替换后照原样生效。
    for (HWND control : { m_hNumberPos, m_hNumberSep })
    {
        if (control != nullptr)
        {
            ::EnableWindow(control, TRUE);
        }
    }

    const int extMode = ComboSelection(m_hExtMode);
    if (m_hExtValue != nullptr)
    {
        // 只有选了「修改」才让填扩展名；选「不修改」时灰掉（灰底由 WM_CTLCOLORSTATIC 画）
        ::EnableWindow(m_hExtValue, extMode != 0 ? TRUE : FALSE);
    }
}

// ============================================================================
// 查找方式的 9 条内置正则
//
// 用户不太可能背得住正则语法，所以内置一批"日常改名真用得上"的写法，
// 在下拉的「查找方式」里选一条，就把写法直接填进「查找」。
// 下拉第 1 项是「普通查找」（= 不按正则），第 2~10 项是下面这 9 条，
// 最后第 11 项是「自定义正则」，让用户自己在查找框里写。
// 除「普通查找」以外，一律按正则解释。
// ============================================================================

// ---- 内置正则写法表（两列：写法 / 说明）----
// 上表放在匿名命名空间里，避免和别的编译单元撞名；只在下面几个函数里用。
namespace
{
    const BatchRenameDialog::RegexEntry kRegexEntries[] =
    {
        // ---- 常用写法：用户手里真会用来改名的 8 条（编号跟旧版一致，模板文件里存的就是它们）----
        {  1, L"\\d+",                  L"连续数字（照片 001 里的 001）",        true },
        {  2, L"\\s*\\(\\d+\\)$",       L"结尾的副本编号 (N)，连前面的空格一起", true },
        {  3, L"^\\d+[\\.\\-_ ]*",      L"开头的编号，如 01_、001-、02 ",        true },
        {  4, L"\\s+",                  L"连续空白（空格 / Tab 串）",            true },
        {  5, L"\\d{8}",                L"8 位日期（20260105）",                 true },
        {  6, L"\\([^)]*\\)",           L"半角括号备注（(草稿)）",               true },
        {  7, L"【[^】]*】",             L"全角方括号备注（【草稿】）",           true },
        {  8, L"\\d+\\.\\d+(\\.\\d+)*",  L"版本号（1.2 / 1.2.3）",               true },

        // ---- 语法元素：照「正则速查表」补的，20 起编号 ----
        { 20, L".",                     L"任意单个字符",                         false },
        { 21, L".*",                    L"0 个或以上个任意字符",                 false },
        { 22, L".+",                    L"一个或以上任意字符",                   false },
        { 23, L".+?",                   L"一个或以上任意字符，但尽可能少",       false },
        { 24, L".{3}",                  L"正好三个任意字符",                     false },
        { 25, L".{3,}",                 L"三个或以上任意字符",                   false },
        { 26, L".{2,4}",                L"二到四个任意字符",                     false },
        { 27, L"(.+)",                  L"子表达式分组（一个或多个任意字符）",   false },
        { 28, L"[abc]",                 L"括号里任意一个字符（a 或 b 或 c）",    false },
        { 29, L"[^abc]",                L"除了这些以外的一个字符",               false },
        { 30, L"[a-z]",                 L"范围内的一个字符（a 到 z）",           false },
        { 31, L"aa|bb|cc",              L"候选之一（aa 或 bb 或 cc）",           false },
        { 32, L"\\\\",                  L"匹配一个真正的反斜杠 \\",              false },
        { 33, L"\\w",                   L"一个字母 / 数字 / 下划线",             false },
        { 34, L"\\s",                   L"一个空白字符（空格 / Tab / 换行）",    false },
        { 35, L"\\b",                   L"单词边界",                             false },
        { 36, L"\\A",                   L"整段文本的开头",                       false },
        { 37, L"\\Z",                   L"整段文本的结尾",                       false }
    };

    // 表里一共多少条
    const size_t kRegexEntryCount = _countof(kRegexEntries);
}

size_t BatchRenameDialog::RegexEntryCount()
{
    return kRegexEntryCount;
}

const BatchRenameDialog::RegexEntry* BatchRenameDialog::FindRegexEntry(int id)
{
    for (size_t i = 0; i < kRegexEntryCount; ++i)
    {
        if (kRegexEntries[i].id == id)
        {
            return &kRegexEntries[i];
        }
    }

    return nullptr;
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

    // 先把 10 个默认空槽摆上，读到文件内容再一个个盖掉。
    // 这一步必须放在读文件之前：文件不存在（第一次用）时下面会直接 return，
    // 那时如果 m_templates 还是空的，模板按钮一个都画不出来、也点不动。
    EnsureTemplateSlots();

    const std::wstring path = TemplateFilePath();
    if (path.empty())
    {
        return;
    }

    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr)
    {
        return;   // 第一次用，还没有这个文件，算正常（用刚补上的 10 个空槽）
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
    bool           legacyRegexChecked = false;   // 老格式里"查找按正则处理"那个勾的值

    // 文件里的模板先装在这里，读完再覆盖到固定的 10 个槽上
    // （不能直接往 m_templates 后面 push，那会变成 20 个槽）
    std::vector<RenameTemplate> loaded;

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
                // 老文件里只有 regex=1/0、没有 regexpreset：
                // 勾过正则的槽按"自定义正则"恢复（查找框里的写法原样留着）
                if (legacyRegexChecked && current.regexPreset == 0)
                {
                    current.regexPreset = kRegexCustomItem;
                }

                loaded.push_back(current);
            }

            current    = {};
            hasCurrent = false;
            legacyRegexChecked = false;
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
                else if (key == L"inclext")
                {
                    current.includeExt = (value == L"1");
                }
                else if (key == L"regexpreset")
                {
                    current.regexPreset = ::_wtoi(value.c_str());
                }
                else if (key == L"regex")
                {
                    // 老格式（勾选框时代）只有 0/1，先在旁边记一笔，
                    // 整条模板读完再看要不要按"自定义正则"处理
                    legacyRegexChecked = (value == L"1");
                }
                else if (key == L"number")
                {
                    current.number = (value == L"1");
                }
                else if (key == L"numberreplace")
                {
                    current.numberReplace = (value == L"1");
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
        if (legacyRegexChecked && current.regexPreset == 0)
        {
            current.regexPreset = kRegexCustomItem;
        }

        loaded.push_back(current);
    }

    // 读到的模板从上往下填进前几个槽，多出来的丢掉，缺的还是默认空槽
    for (size_t i = 0; i < loaded.size() && i < m_templates.size(); ++i)
    {
        m_templates[i] = loaded[i];
    }

    EnsureTemplateSlots();
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
        text += std::wstring(L"inclext=") + (item.includeExt ? L"1" : L"0") + L"\r\n";
        text += L"regexpreset=" + std::to_wstring(item.regexPreset) + L"\r\n";

        // 老字段顺手写上：万一用户换回旧版程序，至少"是否按正则"还能对上
        text += std::wstring(L"regex=") + ((item.regexPreset != 0) ? L"1" : L"0") + L"\r\n";
        text += std::wstring(L"number=") + (item.number ? L"1" : L"0") + L"\r\n";
        text += std::wstring(L"numberreplace=") + (item.numberReplace ? L"1" : L"0") + L"\r\n";
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

// ============================================================================
// 模板槽：固定 10 个
//
// 设计成"一排槽"而不是"用户自己新建"：
//   · 打开窗口时 10 个槽都在，名字叫「模板1…模板10」，参数全空；
//   · 左键点一个槽 = 高亮它 + 把它的参数载入规则区；
//   · 右键点一个槽 = 在按钮上原地改名；
//   · 改过参数没保存时「应用」是灰的，先点「保存」才能应用。
// ============================================================================

void BatchRenameDialog::EnsureTemplateSlots()
{
    // 多的截掉，缺的补（文件被手工改坏、或者第一次用都会走到补这一步）
    while (m_templates.size() > kTemplateSlotCount)
    {
        m_templates.pop_back();
    }

    for (size_t i = m_templates.size(); i < kTemplateSlotCount; ++i)
    {
        RenameTemplate slot = {};                 // 参数全默认，等于"什么都没设"
        slot.name = L"模板" + std::to_wstring(i + 1);
        m_templates.push_back(slot);
    }

    for (size_t i = 0; i < m_templates.size(); ++i)
    {
        if (m_templates[i].name.empty())
        {
            m_templates[i].name = L"模板" + std::to_wstring(i + 1);
        }
    }
}

// 只比较"规则"，不比较名字：名字是标识，不算参数改动
static bool SameRules(const RenameTemplate& a, const RenameTemplate& b)
{
    return a.prefix == b.prefix &&
           a.suffix == b.suffix &&
           a.find == b.find &&
           a.replace == b.replace &&
           a.caseSensitive == b.caseSensitive &&
           a.includeExt == b.includeExt &&
           a.regexPreset == b.regexPreset &&
           a.number == b.number &&
           a.numberReplace == b.numberReplace &&
           a.numberStart == b.numberStart &&
           a.numberStep == b.numberStep &&
           a.numberDigits == b.numberDigits &&
           a.numberPos == b.numberPos &&
           a.numberSep == b.numberSep &&
           a.extMode == b.extMode &&
           a.extValue == b.extValue &&
           a.caseMode == b.caseMode &&
           a.sortMode == b.sortMode &&
           a.sortReverse == b.sortReverse;
}

bool BatchRenameDialog::HasSlotChanges() const
{
    if (m_selectedTemplate < 0 || !m_hasBaseline)
    {
        return false;   // 没选模板就不谈"改没改"
    }

    RenameTemplate now = {};
    CaptureCurrentRules(now);

    return !SameRules(now, m_baseline);
}

bool BatchRenameDialog::ApplyButtonEnabled() const
{
    if (!m_regexValid || m_rows.empty())
    {
        return false;
    }

    for (const auto& row : m_rows)
    {
        if (row.state == 2)     // 有冲突
        {
            return false;
        }
    }

    // 选中了模板又改过参数：必须先「保存」，否则改了也白改（模板下次载入还是老样子）
    if (HasSlotChanges())
    {
        return false;
    }

    return true;
}

void BatchRenameDialog::ClearAllRules()
{
    // 结构体默认值就是界面上"什么都没设"的样子，直接套一遍
    RenameTemplate empty = {};
    empty.numberSep = L"-";

    ApplyTemplateToControls(empty);

    m_order.clear();   // 手动排的顺序也一起清掉

    // Recalculate 会把底部统计、冲突提示和四个按钮重画一遍；
    // 高亮的是哪个模板没变，所以模板行不用单独刷
    Recalculate();
}

bool BatchRenameDialog::SaveCurrentSlot()
{
    if (m_selectedTemplate < 0 ||
        m_selectedTemplate >= static_cast<int>(m_templates.size()))
    {
        return false;   // 没选中模板，没什么可保存
    }

    RenameTemplate captured = {};
    CaptureCurrentRules(captured);
    captured.name = m_templates[m_selectedTemplate].name;

    m_templates[m_selectedTemplate] = captured;   // 覆盖这个槽
    m_baseline    = captured;                     // 存完就是"新的干净状态"
    m_hasBaseline = true;

    SaveTemplatesToDisk();

    // 保存完「应用」要从灰变回可点，Recalculate 会把底部那一条重画一遍
    Recalculate();

    return true;
}

bool BatchRenameDialog::ConfirmLeaveSlot()
{
    if (!HasSlotChanges())
    {
        return true;   // 没改过：一声不响就走
    }

    const std::wstring name = (m_selectedTemplate >= 0 &&
                               m_selectedTemplate < static_cast<int>(m_templates.size()))
                                  ? m_templates[m_selectedTemplate].name
                                  : std::wstring(L"当前模板");

    const std::wstring message =
        L"模板「" + name + L"」的参数改过还没保存。\r\n"
        L"先保存的话，下次点这个模板还是这套参数；不保存就直接跳到新模板。";

    // 这里原本用 MessageBox，但它不认"窗口居中"，实测弹到了屏幕右下角（还压到任务栏那边）。
    // 换成 TaskDialog：TDF_POSITION_RELATIVE_TO_WINDOW 会让它在属主窗口（本窗口）中间弹出。
    // 三个按钮自己起名字，返回的自定义 ID 再翻译回"是 / 否 / 取消"。
    const int kButtonSave   = 100;
    const int kButtonLeave  = 101;
    const int kButtonCancel = 102;

    TASKDIALOG_BUTTON buttons[3] =
    {
        { kButtonSave,   L"是，先保存再继续" },
        { kButtonLeave,  L"否，不保存直接继续" },
        { kButtonCancel, L"取消，留在这里继续改" }
    };

    TASKDIALOGCONFIG config = {};
    config.cbSize             = sizeof(config);
    config.hwndParent         = m_hWnd;
    config.dwFlags            = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
    config.dwCommonButtons    = 0;
    config.pszWindowTitle     = L"批量重命名";
    config.pszMainIcon        = TD_INFORMATION_ICON;
    config.pszMainInstruction = L"模板的参数改过还没保存";
    config.pszContent         = message.c_str();
    config.pButtons           = buttons;
    config.cButtons           = ARRAYSIZE(buttons);
    config.nDefaultButton     = kButtonSave;

    int pressed = kButtonCancel;
    ::TaskDialogIndirect(&config, &pressed, nullptr, nullptr);

    if (pressed == kButtonCancel)
    {
        return false;
    }
    if (pressed == kButtonSave)
    {
        SaveCurrentSlot();
    }
    return true;
}

void BatchRenameDialog::OnTemplateSlotClick(int index)
{
    if (index < 0 || index >= static_cast<int>(m_templates.size()))
    {
        return;
    }

    // 再点一次已经高亮的槽 = 取消选中，回到"没选任何模板"的状态
    // （这时底部「保存」消失、「应用」恢复可点；规则区里已经填的参数原样留着，
    //   用户可能是想摆脱模板束缚直接改一版去应用）
    if (index == m_selectedTemplate)
    {
        if (!ConfirmLeaveSlot())
        {
            return;   // 用户在三选里选了"取消"，那就保持选中不动
        }

        const int previous = m_selectedTemplate;

        m_selectedTemplate = -1;
        m_hasBaseline      = false;   // 没有模板了，也就不存在"改没改这个模板"

        // 底部（「保存」消失、「应用」由灰变亮）交给 Recalculate
        Recalculate();

        if (previous >= 0 && previous < static_cast<int>(m_templateRects.size()))
        {
            InvalidatePiece(m_hWnd, m_templateRects[static_cast<size_t>(previous)]);
        }
        return;
    }

    if (!ConfirmLeaveSlot())
    {
        return;   // 上一个模板还有没保存的修改，用户选择"留在"
    }

    const int previous = m_selectedTemplate;

    m_selectedTemplate = index;
    ApplyTemplateToControls(m_templates[index]);

    m_baseline    = m_templates[index];
    m_hasBaseline = true;

    // 底部（「保存」出现、统计变化）由 Recalculate 重画；
    // 模板行只把"原来高亮的"和"现在高亮的"两个槽刷一遍，别整窗刷
    Recalculate();

    if (previous >= 0 && previous < static_cast<int>(m_templateRects.size()))
    {
        InvalidatePiece(m_hWnd, m_templateRects[static_cast<size_t>(previous)]);
    }

    InvalidatePiece(m_hWnd, m_templateRects[static_cast<size_t>(index)]);
}

void BatchRenameDialog::BeginTemplateRename(int index)
{
    if (index < 0 || index >= static_cast<int>(m_templateRects.size()))
    {
        return;
    }

    if (m_hRenameEdit != nullptr)
    {
        CommitTemplateRename();   // 上一个还没收掉，先收
    }

    const RECT& rect = m_templateRects[index];

    // 就地改名：输入框比按钮本身小一圈（上下左右各缩 1px），
    // 这样按钮的边框、底色还留在外面看得见，感觉是"按钮原地变成可编辑的"，
    // 而不是按钮整个消失、只剩一个白框。
    RECT editRect = rect;
    ::InflateRect(&editRect, -1, -1);

    m_renameIndex = index;
    m_hRenameEdit = ::CreateWindowExW(0, L"EDIT", m_templates[index].name.c_str(),
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                      editRect.left, editRect.top,
                                      editRect.right - editRect.left, editRect.bottom - editRect.top,
                                      m_hWnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRenameEdit)),
                                      ::GetModuleHandleW(nullptr), nullptr);

    if (m_hRenameEdit != nullptr)
    {
        ::SendMessageW(m_hRenameEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);

        ::SendMessageW(m_hRenameEdit, EM_SETSEL, 0, -1);   // 全选，直接打字就替换
        ::SetFocus(m_hRenameEdit);

        UpdateRenameEditLayout();   // 第一下就把字摆到按钮正中

        ::InvalidateRect(m_hWnd, &rect, FALSE);
    }
}

// 让就地改名的输入框看起来"字在按钮正中间"。
// 单行 EDIT 的 ES_CENTER 是不起作用的（那个样式只对多行框有效），靠 EM_SETMARGINS 折算
// 内边距在实测里也不稳（选区会偏左）。所以换个笨办法但可靠：
// 把输入框本身做成"文字宽 + 一点余量"的小方框，再在按钮里水平居中摆，
// 框里是左对齐，整体看上去就跟按钮上的字一样居中；打字时按新文字宽重算一次。
void BatchRenameDialog::UpdateRenameEditLayout()
{
    if (m_hRenameEdit == nullptr)
    {
        return;
    }

    if (m_renameIndex < 0 || m_renameIndex >= static_cast<int>(m_templateRects.size()))
    {
        return;
    }

    const RECT& slot = m_templateRects[static_cast<size_t>(m_renameIndex)];

    // 按钮里留 1px 边，输入框就在这 1px 里面活动
    const int innerLeft   = slot.left + Scale(1);
    const int innerTop    = slot.top + Scale(1);
    const int innerWidth  = (slot.right - slot.left) - Scale(2);
    const int innerHeight = (slot.bottom - slot.top) - Scale(2);

    // 量一下现在这几个字有多宽
    const std::wstring text = GetTextOf(m_hRenameEdit);

    int textWidth = 0;
    {
        HDC dc = ::GetDC(m_hRenameEdit);
        if (dc != nullptr)
        {
            HGDIOBJ oldFont = ::SelectObject(dc, m_font);
            SIZE size = {};
            ::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
            textWidth = size.cx;
            ::SelectObject(dc, oldFont);
            ::ReleaseDC(m_hRenameEdit, dc);
        }
    }

    // 留 6px 余量给光标和选区，超出按钮宽度就顶到边（这时文字自然会长到框外面截断）
    int width = textWidth + Scale(6);
    if (width > innerWidth)
    {
        width = innerWidth;
    }

    // 输入框自己还有 1~2px 的内边距，所以居中后再往右补 Scale(3)，
    // 这样字的视觉中心才正好落在按钮中心上
    int left = innerLeft + (innerWidth - width) / 2 + Scale(3);
    if (left < innerLeft)
    {
        left = innerLeft;
    }
    if (left + width > innerLeft + innerWidth)
    {
        left = innerLeft + innerWidth - width;
    }

    ::MoveWindow(m_hRenameEdit, left, innerTop, width, innerHeight, TRUE);
}

void BatchRenameDialog::CommitTemplateRename()
{
    if (m_hRenameEdit == nullptr)
    {
        return;
    }

    // 先把句柄摘下来，DestroyWindow 会触发 EN_KILLFOCUS 再进来一次，靠它挡住
    HWND edit = m_hRenameEdit;
    const int index = m_renameIndex;

    m_hRenameEdit = nullptr;
    m_renameIndex = -1;

    std::wstring text = GetTextOf(edit);
    ::DestroyWindow(edit);

    // 去掉首尾空格
    const size_t first = text.find_first_not_of(L" \t\r\n");
    const size_t last  = text.find_last_not_of(L" \t\r\n");

    if (first == std::wstring::npos)
    {
        text.clear();
    }
    else
    {
        text = text.substr(first, last - first + 1);
    }

    if (index >= 0 && index < static_cast<int>(m_templates.size()) &&
        !text.empty() && text != m_templates[index].name)
    {
        m_templates[index].name = text;
        SaveTemplatesToDisk();
    }

    // 只把改过名的那个槽刷一遍
    if (index >= 0 && index < static_cast<int>(m_templateRects.size()))
    {
        InvalidatePiece(m_hWnd, m_templateRects[static_cast<size_t>(index)]);
    }
    else
    {
        ::InvalidateRect(m_hWnd, nullptr, FALSE);
    }
}

void BatchRenameDialog::CancelTemplateRename()
{
    if (m_hRenameEdit == nullptr)
    {
        return;
    }

    HWND edit = m_hRenameEdit;
    const int index = m_renameIndex;

    m_hRenameEdit = nullptr;
    m_renameIndex = -1;

    ::DestroyWindow(edit);

    // 取消改名：把那个槽的按钮重新画回来（只刷这一块）
    if (index >= 0 && index < static_cast<int>(m_templateRects.size()))
    {
        InvalidatePiece(m_hWnd, m_templateRects[static_cast<size_t>(index)]);
    }
}

void BatchRenameDialog::CaptureCurrentRules(RenameTemplate& out) const
{
    // 名字由调用方决定（槽名不跟着规则走）
    out.prefix  = GetTextOf(m_hPrefix);
    out.suffix  = GetTextOf(m_hSuffix);
    out.find    = GetTextOf(m_hFind);
    out.replace = GetTextOf(m_hReplace);

    out.caseSensitive = (m_hCaseSensitive != nullptr) &&
                        (::SendMessageW(m_hCaseSensitive, BM_GETCHECK, 0, 0) == BST_CHECKED);

    // 英文 naming：includeExt 就是界面上「含扩展名」那个勾
    out.includeExt = (m_hIncludeExt != nullptr) &&
                     (::SendMessageW(m_hIncludeExt, BM_GETCHECK, 0, 0) == BST_CHECKED);

    // 查找方式：0 普通查找 / 1~9 内置正则 / 10 自定义正则
    out.regexPreset = m_regexPreset;

    out.number       = (m_hNumberEnable != nullptr) &&
                       (::SendMessageW(m_hNumberEnable, BM_GETCHECK, 0, 0) == BST_CHECKED);
    out.numberReplace = (m_hNumberReplace != nullptr) &&
                        (::SendMessageW(m_hNumberReplace, BM_GETCHECK, 0, 0) == BST_CHECKED);
    out.numberStart  = static_cast<int>(ReadInteger(m_hNumberStart, 1, 0));
    out.numberStep   = static_cast<int>(ReadInteger(m_hNumberStep, 1, 1));
    out.numberDigits = static_cast<int>(ReadInteger(m_hNumberDigits, 2, 1));
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
    if (m_hIncludeExt != nullptr)
    {
        ::SendMessageW(m_hIncludeExt, BM_SETCHECK,
                       tpl.includeExt ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    // 查找方式：只认 0（普通）/ 10（自定义）/ 表里的写法，其它（比如老文件里的怪值）一律当普通查找
    {
        int preset = tpl.regexPreset;
        if (preset != 0 && preset != kRegexCustomItem && FindRegexEntry(preset) == nullptr)
        {
            preset = 0;
        }

        m_regexPreset = preset;
        ::InvalidateRect(m_hWnd, &m_regexButtonRect, FALSE);   // 按钮上的字要跟着变
    }
    if (m_hNumberEnable != nullptr)
    {
        ::SendMessageW(m_hNumberEnable, BM_SETCHECK,
                       tpl.number ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (m_hNumberReplace != nullptr)
    {
        ::SendMessageW(m_hNumberReplace, BM_SETCHECK,
                       tpl.numberReplace ? BST_CHECKED : BST_UNCHECKED, 0);
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

    // 先记住"哪些行原来是选中的"（按名字记，因为重排之后行号对应的东西就变了）
    std::vector<std::wstring> selectedNames;

    if (m_hList != nullptr)
    {
        for (int i = 0; i < ListView_GetItemCount(m_hList); ++i)
        {
            if ((ListView_GetItemState(m_hList, i, LVIS_SELECTED) & LVIS_SELECTED) != 0 &&
                static_cast<size_t>(i) < m_rows.size())
            {
                selectedNames.push_back(m_rows[static_cast<size_t>(i)].oldName);
            }
        }
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

    // 拖完顺序变了，原来选中的那几行按"原名"重新选上（不然选择会跳到别的文件上）
    if (m_hList != nullptr && !selectedNames.empty())
    {
        const int rowCount = ListView_GetItemCount(m_hList);

        for (int i = 0; i < rowCount; ++i)
        {
            const bool selected =
                (static_cast<size_t>(i) < m_rows.size()) &&
                std::find(selectedNames.begin(), selectedNames.end(),
                          m_rows[static_cast<size_t>(i)].oldName) != selectedNames.end();

            ListView_SetItemState(m_hList, i, selected ? LVIS_SELECTED : 0, LVIS_SELECTED);
        }

        ::SetFocus(m_hList);
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

// 查找是不是按正则走：除了「普通查找」以外，全都是正则
bool BatchRenameDialog::RegexOn() const
{
    return m_regexPreset != 0;
}

// 改查找方式：选了内置正则（1~9）就顺手把那条写法填进「查找」；
// 0（普通查找）和 10（自定义正则）不动用户已经敲的内容
void BatchRenameDialog::SetRegexPreset(int preset)
{
    // 合法编号只有三种：0 = 普通查找、10 = 自定义正则、表里那几条写法
    if (preset != 0 && preset != kRegexCustomItem && FindRegexEntry(preset) == nullptr)
    {
        preset = 0;
    }

    m_regexPreset = preset;

    // 选了表里的写法就把它填进「查找」；普通查找和自定义正则不动用户已经敲的内容
    if (const RegexEntry* entry = FindRegexEntry(preset))
    {
        m_suppressRecalculate = true;

        if (m_hFind != nullptr)
        {
            ::SetWindowTextW(m_hFind, entry->pattern);
        }

        m_suppressRecalculate = false;
    }

    // 按钮上的字要从「普通」变成「正则」（或反过来），只刷这一小块
    InvalidatePiece(m_hWnd, m_regexButtonRect);

    Recalculate();
}

// ----------------------------------------------------------------------------
// 自绘菜单项：左边一列「写法」（等宽字体，看着像代码），右边一列「说明」。
// 这样跟用户给的速查表一个格式：一眼能看出这条正则长什么样、是干什么的。
// ----------------------------------------------------------------------------
void BatchRenameDialog::DrawRegexMenuItem(DRAWITEMSTRUCT* draw)
{
    if (draw == nullptr || draw->itemData == 0)
    {
        return;
    }

    const RegexEntry* entry = reinterpret_cast<const RegexEntry*>(draw->itemData);

    // 鼠标停在这一项上时用系统高亮色，平时用菜单底色（跟着系统主题走）
    const bool hot = (draw->itemState & ODS_SELECTED) != 0;
    const COLORREF backColor = hot ? ::GetSysColor(COLOR_HIGHLIGHT) : ::GetSysColor(COLOR_MENU);
    const COLORREF textColor = hot ? ::GetSysColor(COLOR_HIGHLIGHTTEXT) : ::GetSysColor(COLOR_MENUTEXT);

    HBRUSH brush = ::CreateSolidBrush(backColor);
    ::FillRect(draw->hDC, &draw->rcItem, brush);
    ::DeleteObject(brush);

    ::SetBkMode(draw->hDC, TRANSPARENT);
    ::SetTextColor(draw->hDC, textColor);

    // 左列前面留一小段放"当前用的是哪条"的勾
    const int patternX = draw->rcItem.left + Scale(20);
    const int descX    = draw->rcItem.left + Scale(150);

    if (entry->id == m_regexPreset)
    {
        RECT checkRect = draw->rcItem;
        checkRect.left  += Scale(3);
        checkRect.right  = checkRect.left + Scale(14);
        ::DrawTextW(draw->hDC, L"✓", -1, &checkRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // 左列：写法（等宽字体。按界面的字体高度现做一个，菜单不是常驻，不用缓存）
    LOGFONTW logFont = {};
    HFONT uiFont = reinterpret_cast<HFONT>(::SendMessageW(m_hWnd, WM_GETFONT, 0, 0));
    if (uiFont == nullptr)
    {
        uiFont = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    }

    if (::GetObjectW(uiFont, sizeof(logFont), &logFont) == 0)
    {
        logFont.lfHeight = -Scale(12);
    }

    HFONT monoFont = ::CreateFontW(logFont.lfHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    HGDIOBJ oldFont = ::SelectObject(draw->hDC, (monoFont != nullptr) ? monoFont : uiFont);

    RECT rcPattern = draw->rcItem;
    rcPattern.left  = patternX;
    rcPattern.right = descX - Scale(8);

    ::DrawTextW(draw->hDC, entry->pattern, -1, &rcPattern,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    ::SelectObject(draw->hDC, uiFont);
    ::SetTextColor(draw->hDC, textColor);

    // 右列：说明
    RECT rcDesc = draw->rcItem;
    rcDesc.left  = descX;
    rcDesc.right -= Scale(8);

    ::DrawTextW(draw->hDC, entry->desc, -1, &rcDesc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    ::SelectObject(draw->hDC, oldFont);

    if (monoFont != nullptr)
    {
        ::DeleteObject(monoFont);
    }
}

// 点「普通 / 正则」按钮：弹出菜单。
// 菜单结构 = 普通查找 ／ 常用写法（子菜单）／ 语法元素（子菜单）／ 自定义正则，
// 两个子菜单里的条目都是自绘两列（左写法、右说明）。
void BatchRenameDialog::OnRegexButtonClick()
{
    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr)
    {
        return;
    }

    // 菜单项 ID 用 7000 起，避开所有控件 ID
    const int kMenuBase = 7000;

    ::AppendMenuW(menu, MF_STRING, kMenuBase + 0, L"普通查找（按文字找）");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU commonMenu = ::CreatePopupMenu();
    HMENU syntaxMenu = ::CreatePopupMenu();

    for (size_t i = 0; i < kRegexEntryCount; ++i)
    {
        // 自绘项：把这一条的指针当"附加数据"塞进去，WM_DRAWITEM 里再取出来画
        ::AppendMenuW(kRegexEntries[i].common ? commonMenu : syntaxMenu,
                      MF_OWNERDRAW,
                      static_cast<UINT_PTR>(kMenuBase + kRegexEntries[i].id),
                      reinterpret_cast<LPTSTR>(const_cast<RegexEntry*>(&kRegexEntries[i])));
    }

    ::AppendMenuW(menu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(commonMenu), L"常用写法");
    ::AppendMenuW(menu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(syntaxMenu), L"语法元素（速查表）");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuBase + kRegexCustomItem, L"自定义正则（自己写）");

    // 当前选的那条打勾（自绘项自己画勾，这两条普通项交给系统画）
    ::CheckMenuItem(menu, static_cast<UINT>(kMenuBase + m_regexPreset), MF_BYCOMMAND | MF_CHECKED);

    // 菜单贴在按钮左下角弹出（按钮下方空间不够时系统会自动往上翻）
    const int screenX = m_regexButtonRect.left;
    const int screenY = m_regexButtonRect.bottom;

    POINT point = { screenX, screenY };
    ::ClientToScreen(m_hWnd, &point);

    const int command = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                         point.x, point.y, 0, m_hWnd, nullptr);

    ::DestroyMenu(menu);   // 连子菜单一起销毁

    if (command >= kMenuBase)
    {
        SetRegexPreset(command - kMenuBase);
    }
}

std::wstring BatchRenameDialog::ComputeNewName(size_t index, const std::wstring& originalName) const
{
    std::wstring stem;
    std::wstring extension;
    SplitFileName(originalName, stem, extension);

    // 0) 先把「扩展名」这个下拉定下来（保留 / 改成指定 / 去掉）
    //    放在查找替换前面，是为了让勾了「含扩展名」的时候，查找看到的是"最终要用的那个扩展名"
    switch (ComboSelection(m_hExtMode))
    {
    case 1:   // 修改：右边框填什么就用什么，没填就是不要扩展名
    {
        std::wstring value = GetTextOf(m_hExtValue);
        if (value.empty())
        {
            extension.clear();   // 「修改」但空着 = 去掉扩展名
        }
        else
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

    // 勾了「含扩展名」就把扩展名一起并进"要处理的那一段"；
    // 不勾就只动主文件名（默认行为，扩展名原样保留）
    const bool includeExt = (m_hIncludeExt != nullptr) &&
                            (::SendMessageW(m_hIncludeExt, BM_GETCHECK, 0, 0) == BST_CHECKED);

    std::wstring working = includeExt ? (stem + extension) : stem;

    // 1) 查找替换（普通文字，或按正则处理）
    const std::wstring find = GetTextOf(m_hFind);
    if (!find.empty())
    {
        // 除了「普通查找」以外都按正则解释（内置预设和自定义正则都算）
        const bool useRegex = RegexOn();

        const std::wstring replacement = GetTextOf(m_hReplace);

        if (useRegex)
        {
            // 正则的合法性在外面已经编译验证过（m_regexValid），这里直接用缓存的那份
            if (m_regexValid && EnsureRegexCompiled(find))
            {
                try
                {
                    working = std::regex_replace(working, g_regexCache.regex, replacement);
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

            working = ReplaceAll(working, find, replacement, caseSensitive);
        }
    }

    // 2) 大小写（作用范围跟查找替换一致）
    switch (ComboSelection(m_hCaseMode))
    {
    case 1:   // 全部大写
        ::CharUpperBuffW(working.data(), static_cast<DWORD>(working.size()));
        break;

    case 2:   // 全部小写
        ::CharLowerBuffW(working.data(), static_cast<DWORD>(working.size()));
        break;

    case 3:   // 首字母大写（其余转小写）
        if (!working.empty())
        {
            ::CharLowerBuffW(working.data(), static_cast<DWORD>(working.size()));
            working[0] = static_cast<wchar_t>(::towupper(working[0]));
        }
        break;

    default:
        break;
    }

    if (includeExt)
    {
        // 替换可能把扩展名换掉、甚至把那个点删了，所以这里按处理后的结果重新拆一次
        SplitFileName(working, stem, extension);
    }
    else
    {
        // 没勾「含扩展名」：extension 还是第 0 步算好的那份，主名直接用 working
        stem = working;
    }

    // 3) 序号（只有勾了才加；编号按排序后的次序给）
    const bool numberOn = (m_hNumberEnable != nullptr) &&
                          (::SendMessageW(m_hNumberEnable, BM_GETCHECK, 0, 0) == BST_CHECKED);

    if (numberOn)
    {
        const long long start  = ReadInteger(m_hNumberStart, 1, 0);
        const long long step   = ReadInteger(m_hNumberStep, 1, 1);
        // 位数框空着就用 2（跟框里那句灰色提示一致，免得"提示写 2、实际出 1 位"）
        const int       digits = static_cast<int>(ReadInteger(m_hNumberDigits, 2, 1));

        const std::wstring numberText =
            FormatNumber(start + step * static_cast<long long>(index), digits);

        const bool replaceOn = (m_hNumberReplace != nullptr) &&
                               (::SendMessageW(m_hNumberReplace, BM_GETCHECK, 0, 0) == BST_CHECKED);

        if (replaceOn)
        {
            // 替换：原文件名整段不要了，新名字就是序号本身；
            // 但「分隔符」仍然按「位置」贴在序号的一侧（分隔符留空就是干净的 01）
            const std::wstring separator = GetTextOf(m_hNumberSep);

            if (ComboSelection(m_hNumberPos) == 1)
            {
                stem = separator + numberText;            // 位置=放在名字前面 → -01
            }
            else
            {
                stem = numberText + separator;            // 位置=放在名字后面 → 01-
            }
        }
        else
        {
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
    }

    // 4) 前缀 / 后缀（加在整理好的主名两端；扩展名始终留在最后，不参与这两项）
    stem = GetTextOf(m_hPrefix) + stem + GetTextOf(m_hSuffix);

    return stem + extension;
}

void BatchRenameDialog::BuildPreviewRows()
{
    m_rows.clear();

    // ---- 正则先验证一次：写法有误就整张表标红，别让用户以为能改 ----
    const std::wstring findText = GetTextOf(m_hFind);
    const bool useRegex = RegexOn();

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

    // 只重画底部那一条：之前整窗 InvalidateRect，
    // 父窗口会先把整块刷白再等子控件重画，输入一个字就闪一下。
    // 这一块从参数区下面那条空白（冲突提示写在那里）一直盖到底部按钮，
    // 因为「应用」的灰/亮、「保存」的出现、统计文字都在这块里变。
    if (m_hWnd != nullptr)
    {
        RECT bottomRect = {};

        bottomRect.left   = Scale(kRuleTop);
        bottomRect.top    = Scale(kRuleTop) + Scale(kRuleRowHeight) * 2 + Scale(kRuleBandHeight);
        bottomRect.right  = m_applyRect.right;
        bottomRect.bottom = m_applyRect.bottom + Scale(8);

        ::InvalidateRect(m_hWnd, &bottomRect, FALSE);
        ::UpdateWindow(m_hWnd);
    }
}

// ============================================================================
// 画窗口
// ============================================================================

void BatchRenameDialog::DrawButton(HDC dc, const RECT& rect, const wchar_t* text,
                                   bool hot, bool pressed, bool primary, bool enabled)
{
    COLORREF face   = kButtonFace;
    COLORREF border = kButtonBorder;

    if (!enabled)
    {
        // 不可点：浅灰底 + 灰字，一眼能看出"现在不能点"
        face   = RGB(246, 247, 249);
        border = RGB(219, 221, 225);
    }
    else if (primary)
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

    if (!enabled)
    {
        ::SetTextColor(dc, RGB(160, 163, 168));
    }
    else
    {
        ::SetTextColor(dc, primary ? RGB(255, 255, 255) : kTitleColor);
    }

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
    ::DrawTextW(dc, L"按 Ctrl 可多选，支持拖拽排序。单击模板按钮可载入模板，右键单击给模板重命名。",
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

    // 参数区三行：每行用一个描边矩形"框成一组"（白底不填色，免得系统复选框刷出白块）。
    // 行与行之间靠 48 - 36 = 12px 的白缝分开，框线只是把这一组圈起来。
    {
        HBRUSH groupBorderBrush = ::CreateSolidBrush(kRuleBandBorder);

        for (int row = 0; row < 3; ++row)
        {
            RECT band = { padding - Scale(kRuleBandInset),
                          Scale(kRuleTop) + Scale(kRuleRowHeight) * row,
                          clientRect.right - padding + Scale(kRuleBandInset),
                          Scale(kRuleTop) + Scale(kRuleRowHeight) * row + Scale(kRuleBandHeight) };

            ::FrameRect(dc, &band, groupBorderBrush);
        }

        ::DeleteObject(groupBorderBrush);
    }

    // ---- 第三行右侧的排序格：用一条竖线跟左边的"序号"部分隔开，等于单独圈成一格 ----
    {
        const int rowTop    = Scale(kRuleTop) + Scale(kRuleRowHeight) * 2;
        const int rowBottom = rowTop + Scale(kRuleBandHeight);

        ::MoveToEx(dc, Scale(kRuleSortDividerX), rowTop + Scale(1), nullptr);
        ::LineTo(dc, Scale(kRuleSortDividerX), rowBottom - Scale(1));
    }

    ::SelectObject(dc, oldPen);
    ::DeleteObject(dividerPen);

    // ---- 模板栏：10 个槽一行排满（左键载入、右键原地改名）----
    {
        ::SelectObject(dc, m_font);
        ::SetTextColor(dc, kTitleColor);

        for (size_t i = 0; i < m_templateRects.size(); ++i)
        {
            DrawTemplateSlot(dc, m_templateRects[i], static_cast<int>(i));
        }

        // 10 个槽正好占满一行，右边没有多余地方放说明，用法写在顶部副标题里
    }

    // ---- 参数区标签：和 Layout 用同一份 kRuleSpecs，标签正好落在对应控件左边 ----
    ::SelectObject(dc, m_font);
    ::SetTextColor(dc, kTitleColor);

    for (int i = 0; i < kRfCount; ++i)
    {
        const RuleFieldSpec& spec = kRuleSpecs[i];
        if (spec.label == nullptr)
        {
            continue;
        }

        const int rowTop = Scale(kRuleTop) + Scale(kRuleRowHeight) * spec.row;
        const int ctlTop = rowTop + Scale(kRuleControlTop);

        // 标签右对齐到"控件左边再往左 5px"，这样每个标签和它的控件间距都是 5px，
        // 也不受标签字数影响（"前缀"两个字和"分隔符"三个字看上去一样整齐）。
        RECT labelRect = { Scale(spec.labelX), ctlTop,
                           Scale(spec.labelX) + Scale(spec.labelWidth),
                           ctlTop + Scale(kRuleControlHeight) };

        ::DrawTextW(dc, spec.label, -1, &labelRect,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // ---- 查找方式按钮：文字随选中项变（普通 / 正则）----
    ::SelectObject(dc, m_font);
    DrawButton(dc, m_regexButtonRect,
               (m_regexPreset == 0) ? L"普通" : L"正则",
               m_hotRegexButton, m_pressedRegexButton, false);

    // ---- 底部统计（和四个按钮同一条线，在左边空白里居中）----
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
    ::DrawTextW(dc, statsText, -1, &m_statsRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // 有冲突时「应用」不可用，在参数区和底部按钮之间那条窄缝里说一句原因
    // （表格加高 47px 之后这条缝只剩十几像素，所以把提示压成一行，贴着按钮上沿画）
    if (conflictCount > 0)
    {
        const int bandBottom = Scale(kRuleTop) + Scale(kRuleRowHeight) * 2 + Scale(kRuleBandHeight);

        RECT warnRect = { padding, bandBottom + Scale(1),
                          clientRect.right - padding,
                          m_applyRect.top - Scale(1) };

        ::SetTextColor(dc, kErrorColor);
        ::DrawTextW(dc, L"有冲突的行会标红，改到没有冲突才能应用。", -1, &warnRect,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    ::SelectObject(dc, oldFont);

    // ---- 底部四个按钮：取消 / 清空 / 保存 / 应用 ----
    // 「保存」只在选中了某个模板槽时出现（没选中就没有"保存到哪"的说法）
    DrawButton(dc, m_cancelRect, L"取消", m_hotCancel, m_pressedCancel, false);
    DrawButton(dc, m_clearRect,  L"清空", m_hotClear,  m_pressedClear,  false);

    if (m_selectedTemplate >= 0)
    {
        DrawButton(dc, m_saveRect, L"保存", m_hotSave, m_pressedSave, false);
    }

    DrawButton(dc, m_applyRect, L"应用", m_hotApply, m_pressedApply, true, ApplyButtonEnabled());
}

// 画一个模板槽：选中的用蓝框 + 浅蓝底，鼠标悬停用浅灰底
void BatchRenameDialog::DrawTemplateSlot(HDC dc, const RECT& rect, int index)
{
    if (index < 0 || index >= static_cast<int>(m_templates.size()))
    {
        return;
    }

    const bool selected = (index == m_selectedTemplate);
    const bool hot      = (index == m_hotTemplate);
    const bool pressed  = (index == m_pressedTemplate);

    COLORREF face   = kButtonFace;
    COLORREF border = kButtonBorder;
    COLORREF text   = kTitleColor;

    if (selected)
    {
        face   = RGB(232, 240, 252);    // 浅蓝底
        border = kPrimaryFace;          // 主色描边
    }
    else if (hot)
    {
        face   = kButtonHotFace;
        border = kButtonHotBorder;
    }

    if (pressed)
    {
        face = kButtonDownFace;
    }

    HBRUSH brush = ::CreateSolidBrush(face);
    HPEN   pen   = ::CreatePen(PS_SOLID, 1, border);

    HGDIOBJ oldBrush = ::SelectObject(dc, brush);
    HGDIOBJ oldPen   = ::SelectObject(dc, pen);

    ::Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);

    ::SelectObject(dc, oldPen);
    ::SelectObject(dc, oldBrush);
    ::DeleteObject(pen);
    ::DeleteObject(brush);

    // 就地改名时按钮被输入框盖住，文字不用画
    if (m_hRenameEdit != nullptr && index == m_renameIndex)
    {
        return;
    }

    HGDIOBJ oldFont = ::SelectObject(dc, m_font);
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, selected ? kPrimaryFace : text);

    RECT textRect = rect;
    textRect.left  += Scale(6);
    textRect.right -= Scale(6);

    ::DrawTextW(dc, m_templates[index].name.c_str(), -1, &textRect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    ::SelectObject(dc, oldFont);
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

        // 双缓冲：先整块画到内存位图，再一次性贴到窗口。
        // 这个窗口大半是自绘的（按钮、模板槽、统计文字），每个按钮都要新建
        // 画刷和画笔，如果直接画到窗口上，鼠标滑过去就是清清楚楚一片白闪。
        RECT client = {};
        ::GetClientRect(hWnd, &client);

        const int width  = client.right - client.left;
        const int height = client.bottom - client.top;

        HDC     memDc     = ::CreateCompatibleDC(dc);
        HBITMAP bitmap    = nullptr;
        HGDIOBJ oldBitmap = nullptr;

        if (memDc != nullptr && width > 0 && height > 0)
        {
            bitmap = ::CreateCompatibleBitmap(dc, width, height);
        }

        if (bitmap != nullptr)
        {
            oldBitmap = ::SelectObject(memDc, bitmap);

            // 内存位图先用白底铺满，保证没有上一帧的残影
            ::FillRect(memDc, &client, m_whiteBrush);
            Draw(memDc);

            // 只把真正需要更新的那一块贴到屏幕上（有 WS_CLIPCHILDREN，
            // 这块区域不会落在子控件上，所以不会盖掉列表和输入框）
            const RECT& dirty = paintStruct.rcPaint;

            if (dirty.right > dirty.left && dirty.bottom > dirty.top)
            {
                ::BitBlt(dc, dirty.left, dirty.top,
                         dirty.right - dirty.left, dirty.bottom - dirty.top,
                         memDc, dirty.left, dirty.top, SRCCOPY);
            }

            ::SelectObject(memDc, oldBitmap);
            ::DeleteObject(bitmap);
            ::DeleteDC(memDc);
        }
        else
        {
            // 退路：内存位图建不出来（极端情况），直接把内容画到窗口上
            if (memDc != nullptr)
            {
                ::DeleteDC(memDc);
            }
            Draw(dc);
        }

        ::EndPaint(hWnd, &paintStruct);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;   // 背景在 WM_PAINT 里画，别闪

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND child = reinterpret_cast<HWND>(lParam);
        const int childId = (child != nullptr) ? ::GetDlgCtrlID(child) : 0;

        // 复选框（含扩展名 / 区分大小写 / 加序号 / 替换 / 倒序）也会走这条消息，
        // 但它们没有"填什么"的意思，禁用时只把字压淡、底色始终白（否则会冒灰块）
        const bool isCheckBox = (childId == kIdIncludeExt || childId == kIdCaseSensitive ||
                                 childId == kIdNumberEnable || childId == kIdNumberReplace ||
                                 childId == kIdSortReverse);

        if (isCheckBox)
        {
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, (child != nullptr && ::IsWindowEnabled(child) == FALSE)
                                   ? RGB(150, 153, 158)
                                   : kTitleColor);
            return reinterpret_cast<LRESULT>(m_whiteBrush);
        }

        // 被禁用的输入框（没勾「加序号」时的起始/步长等、扩展名下拉选了「不修改」时的那个框）：
        // 灰底灰字，一眼看出"现在不能填"。别的控件照旧白底。
        if (child != nullptr && ::IsWindowEnabled(child) == FALSE)
        {
            ::SetBkMode(dc, OPAQUE);
            ::SetBkColor(dc, RGB(240, 240, 240));
            ::SetTextColor(dc, RGB(150, 153, 158));
            return reinterpret_cast<LRESULT>(m_disabledBrush);
        }

        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, kTitleColor);
        return reinterpret_cast<LRESULT>(m_whiteBrush);
    }

    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND child = reinterpret_cast<HWND>(lParam);

        ::SetBkMode(dc, TRANSPARENT);

        // 参数区里的复选框没勾「加序号」时会被禁用：这时只把字压淡，
        // 底色仍旧是白的（不然禁用按钮会自己刷一块灰，在白底分组框里很扎眼）
        if (child != nullptr && ::IsWindowEnabled(child) == FALSE)
        {
            ::SetTextColor(dc, RGB(150, 153, 158));
        }
        else
        {
            ::SetTextColor(dc, kTitleColor);
        }

        return reinterpret_cast<LRESULT>(m_whiteBrush);
    }

    case WM_COMMAND:
    {
        const int controlId = LOWORD(wParam);
        const int notification = HIWORD(wParam);

        // ---- 模板就地改名的输入框 ----
        if (controlId == kIdRenameEdit && m_hRenameEdit != nullptr)
        {
            if (notification == EN_KILLFOCUS)
            {
                CommitTemplateRename();   // 点到别处就算确认，不留悬空输入框
            }
            else if (notification == EN_CHANGE)
            {
                UpdateRenameEditLayout();   // 边打字边把字保持在按钮正中
            }
            return 0;
        }

        // 回车 / Esc（IsDialogMessage 会翻译成 IDOK / IDCANCEL）
        if (controlId == IDCANCEL)
        {
            // 正在改模板名：Esc 只取消改名，不关窗口
            if (m_hRenameEdit != nullptr)
            {
                CancelTemplateRename();
                return 0;
            }

            if (ConfirmLeaveSlot())
            {
                ::DestroyWindow(hWnd);
            }
            return 0;
        }

        if (controlId == IDOK)
        {
            // 正在改模板名：回车就是确认改名，别顺手把窗口应用掉
            if (m_hRenameEdit != nullptr)
            {
                CommitTemplateRename();
                return 0;
            }

            // 下拉列表正展开的时候，回车的意思是"收下拉"，不能顺手把窗口应用掉
            // （否则用户在「位置」「扩展名」这些下拉里按回车选条目，窗口就被关掉了）
            const HWND combos[] =
            {
                m_hNumberPos, m_hExtMode, m_hCaseMode, m_hSortMode
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
                if (ConfirmLeaveSlot())
                {
                    ::DestroyWindow(hWnd);
                }
                return 0;
            }

            if (controlId == kIdClear)
            {
                ClearAllRules();
                return 0;
            }

            if (controlId == kIdSave)
            {
                SaveCurrentSlot();     // 没选中模板时这个按钮根本不显示，这里等于兜底
                return 0;
            }

            if (controlId == kIdApply)
            {
                // 有冲突、正则在报错、或者"选了模板又改过参数没保存" → 一律不让应用
                if (ApplyButtonEnabled())
                {
                    m_applied = true;
                    ::DestroyWindow(hWnd);
                }
                return 0;
            }
        }

        // 规则控件的任何改动 → 重算预览（查找方式已经不是控件，走的是自绘按钮 + 菜单）
        if (controlId >= kIdPrefix && controlId <= kIdNumberReplace)
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

    case WM_CLOSE:
        // 右上角 ×：同样要照顾"模板改过没保存"
        if (ConfirmLeaveSlot())
        {
            ::DestroyWindow(hWnd);
        }
        return 0;

    case WM_MEASUREITEM:
    {
        // 自绘菜单项全部一样高、一样宽（宽度要够放"写法 + 说明"两列）
        MEASUREITEMSTRUCT* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);

        if (measure != nullptr && measure->CtlType == ODT_MENU)
        {
            measure->itemHeight = Scale(24);
            measure->itemWidth  = Scale(430);   // 够放"写法 + 说明"两列，说明太长时右边省略号
            return TRUE;
        }

        return 0;
    }

    case WM_DRAWITEM:
    {
        DRAWITEMSTRUCT* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);

        if (draw != nullptr && draw->CtlType == ODT_MENU)
        {
            DrawRegexMenuItem(draw);
            return TRUE;
        }

        return 0;
    }

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
        const bool overClear  = (x >= m_clearRect.left && x < m_clearRect.right &&
                                 y >= m_clearRect.top  && y < m_clearRect.bottom);
        const bool overSave   = (m_selectedTemplate >= 0 &&
                                 x >= m_saveRect.left && x < m_saveRect.right &&
                                 y >= m_saveRect.top  && y < m_saveRect.bottom);
        const bool overApply  = (x >= m_applyRect.left && x < m_applyRect.right &&
                                 y >= m_applyRect.top  && y < m_applyRect.bottom);

        // 「普通 / 正则」那个按钮
        const bool overRegex = (x >= m_regexButtonRect.left && x < m_regexButtonRect.right &&
                                y >= m_regexButtonRect.top  && y < m_regexButtonRect.bottom);
        int overSlot = -1;
        for (size_t i = 0; i < m_templateRects.size(); ++i)
        {
            const RECT& rect = m_templateRects[i];

            if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom)
            {
                overSlot = static_cast<int>(i);
                break;
            }
        }

        if (overCancel != m_hotCancel || overClear != m_hotClear ||
            overSave != m_hotSave || overApply != m_hotApply ||
            overRegex != m_hotRegexButton ||
            overSlot != m_hotTemplate)
        {
            // 只刷"状态变了"的那一块，别整窗刷新（整窗刷会连带列表一起白闪）
            if (overCancel != m_hotCancel) { InvalidatePiece(hWnd, m_cancelRect); }
            if (overClear  != m_hotClear)  { InvalidatePiece(hWnd, m_clearRect); }
            if (overSave   != m_hotSave)   { InvalidatePiece(hWnd, m_saveRect); }
            if (overApply  != m_hotApply)  { InvalidatePiece(hWnd, m_applyRect); }
            if (overRegex  != m_hotRegexButton) { InvalidatePiece(hWnd, m_regexButtonRect); }

            if (overSlot != m_hotTemplate)
            {
                if (m_hotTemplate >= 0 &&
                    m_hotTemplate < static_cast<int>(m_templateRects.size()))
                {
                    InvalidatePiece(hWnd, m_templateRects[static_cast<size_t>(m_hotTemplate)]);
                }

                if (overSlot >= 0)
                {
                    InvalidatePiece(hWnd, m_templateRects[static_cast<size_t>(overSlot)]);
                }
            }

            m_hotCancel = overCancel;
            m_hotClear  = overClear;
            m_hotSave   = overSave;
            m_hotApply  = overApply;
            m_hotRegexButton = overRegex;
            m_hotTemplate = overSlot;
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
        if (m_hotCancel || m_hotClear || m_hotSave || m_hotRegexButton ||
            (m_hotApply && ApplyButtonEnabled()) || m_hotTemplate >= 0)
        {
            ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
        }

        return 0;
    }

    case WM_MOUSELEAVE:
    {
        m_trackingLeave = false;

        if (m_hotCancel || m_hotClear || m_hotSave || m_hotApply ||
            m_hotRegexButton || m_hotTemplate >= 0)
        {
            // 同样只刷那几个恢复成普通样子的按钮
            if (m_hotCancel) { InvalidatePiece(hWnd, m_cancelRect); }
            if (m_hotClear)  { InvalidatePiece(hWnd, m_clearRect); }
            if (m_hotSave)   { InvalidatePiece(hWnd, m_saveRect); }
            if (m_hotApply)  { InvalidatePiece(hWnd, m_applyRect); }
            if (m_hotRegexButton) { InvalidatePiece(hWnd, m_regexButtonRect); }

            if (m_hotTemplate >= 0 &&
                m_hotTemplate < static_cast<int>(m_templateRects.size()))
            {
                InvalidatePiece(hWnd, m_templateRects[static_cast<size_t>(m_hotTemplate)]);
            }

            m_hotCancel   = false;
            m_hotClear    = false;
            m_hotSave     = false;
            m_hotApply    = false;
            m_hotRegexButton = false;
            m_hotTemplate = -1;
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
    {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);

        // 模板槽：按下去先记着，松开再算"点中"
        int hitSlot = -1;
        for (size_t i = 0; i < m_templateRects.size(); ++i)
        {
            const RECT& rect = m_templateRects[i];

            if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom)
            {
                hitSlot = static_cast<int>(i);
                break;
            }
        }

        if (hitSlot >= 0)
        {
            m_pressedTemplate = hitSlot;
            ::SetFocus(hWnd);            // 别让改名的输入框的失焦逻辑误伤
            ::SetCapture(hWnd);
            InvalidatePiece(hWnd, m_templateRects[static_cast<size_t>(hitSlot)]);
            return 0;
        }

        const bool overCancel = (x >= m_cancelRect.left && x < m_cancelRect.right &&
                                 y >= m_cancelRect.top  && y < m_cancelRect.bottom);
        const bool overClear  = (x >= m_clearRect.left && x < m_clearRect.right &&
                                 y >= m_clearRect.top  && y < m_clearRect.bottom);
        const bool overSave   = (m_selectedTemplate >= 0 &&
                                 x >= m_saveRect.left && x < m_saveRect.right &&
                                 y >= m_saveRect.top  && y < m_saveRect.bottom);
        const bool overApply  = (x >= m_applyRect.left && x < m_applyRect.right &&
                                 y >= m_applyRect.top  && y < m_applyRect.bottom);
        const bool overRegex  = (x >= m_regexButtonRect.left && x < m_regexButtonRect.right &&
                                 y >= m_regexButtonRect.top  && y < m_regexButtonRect.bottom);

        m_pressedCancel = overCancel;
        m_pressedClear  = overClear;
        m_pressedSave   = overSave;
        m_pressedApply  = overApply;
        m_pressedRegexButton = overRegex;

        if (overRegex)
        {
            ::SetFocus(hWnd);
            ::SetCapture(hWnd);
            InvalidatePiece(hWnd, m_regexButtonRect);
            return 0;
        }

        if (overCancel || overClear || overSave || overApply)
        {
            ::SetCapture(hWnd);

            // 只刷按下去的那一个按钮
            if (overCancel) { InvalidatePiece(hWnd, m_cancelRect); }
            if (overClear)  { InvalidatePiece(hWnd, m_clearRect); }
            if (overSave)   { InvalidatePiece(hWnd, m_saveRect); }
            if (overApply)  { InvalidatePiece(hWnd, m_applyRect); }
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

        // ---- 模板槽：松开时还在同一个槽上才算点中 ----
        const int wasSlot = m_pressedTemplate;
        m_pressedTemplate = -1;

        if (wasSlot >= 0)
        {
            int overSlot = -1;
            for (size_t i = 0; i < m_templateRects.size(); ++i)
            {
                const RECT& rect = m_templateRects[i];

                if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom)
                {
                    overSlot = static_cast<int>(i);
                    break;
                }
            }

            InvalidatePiece(hWnd, m_templateRects[static_cast<size_t>(wasSlot)]);

            if (overSlot == wasSlot)
            {
                OnTemplateSlotClick(wasSlot);
            }
            return 0;
        }

        // ---- 「普通 / 正则」按钮：松开时还在按钮上 → 弹菜单选查找方式 ----
        if (m_pressedRegexButton)
        {
            m_pressedRegexButton = false;
            InvalidatePiece(hWnd, m_regexButtonRect);

            const bool overRegex = (x >= m_regexButtonRect.left && x < m_regexButtonRect.right &&
                                    y >= m_regexButtonRect.top  && y < m_regexButtonRect.bottom);

            if (overRegex)
            {
                ::SetFocus(hWnd);
                OnRegexButtonClick();
            }
            return 0;
        }

        const bool overCancel = (x >= m_cancelRect.left && x < m_cancelRect.right &&
                                 y >= m_cancelRect.top  && y < m_cancelRect.bottom);
        const bool overClear  = (x >= m_clearRect.left && x < m_clearRect.right &&
                                 y >= m_clearRect.top  && y < m_clearRect.bottom);
        const bool overSave   = (m_selectedTemplate >= 0 &&
                                 x >= m_saveRect.left && x < m_saveRect.right &&
                                 y >= m_saveRect.top  && y < m_saveRect.bottom);
        const bool overApply  = (x >= m_applyRect.left && x < m_applyRect.right &&
                                 y >= m_applyRect.top  && y < m_applyRect.bottom);

        const bool wasCancel = m_pressedCancel;
        const bool wasClear  = m_pressedClear;
        const bool wasSave   = m_pressedSave;
        const bool wasApply  = m_pressedApply;

        m_pressedCancel = false;
        m_pressedClear  = false;
        m_pressedSave   = false;
        m_pressedApply  = false;

        // 松开时只把刚才按下去的那个按钮刷回普通样子
        if (wasCancel) { InvalidatePiece(hWnd, m_cancelRect); }
        if (wasClear)  { InvalidatePiece(hWnd, m_clearRect); }
        if (wasSave)   { InvalidatePiece(hWnd, m_saveRect); }
        if (wasApply)  { InvalidatePiece(hWnd, m_applyRect); }

        if (wasCancel && overCancel)
        {
            ::SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(kIdCancel, BN_CLICKED), 0);
        }
        else if (wasClear && overClear)
        {
            ::SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(kIdClear, BN_CLICKED), 0);
        }
        else if (wasSave && overSave)
        {
            ::SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(kIdSave, BN_CLICKED), 0);
        }
        else if (wasApply && overApply)
        {
            ::SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(kIdApply, BN_CLICKED), 0);
        }
        return 0;
    }

    case WM_RBUTTONUP:
    {
        // 模板槽右键 → 就地改名
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);

        for (size_t i = 0; i < m_templateRects.size(); ++i)
        {
            const RECT& rect = m_templateRects[i];

            if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom)
            {
                BeginTemplateRename(static_cast<int>(i));
                return 0;
            }
        }
        return 0;
    }

    case WM_DESTROY:
        m_hWnd        = nullptr;
        m_hRenameEdit = nullptr;   // 子窗口跟着一起销毁了，别留野指针
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
