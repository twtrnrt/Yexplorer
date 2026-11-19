// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/BatchRenameRules.cpp
//
// 批量重命名的规则引擎实现（C8-2 从 BatchRenameDialog.cpp 拆出来）。
//
// 这里只放"怎么把规则套到一个文件名上"的纯计算：
//   · SplitFileName / ReplaceAll / FormatNumber —— 文本处理小工具；
//   · 正则编译缓存 —— 每敲一个字都要重算，不能每次都现场编译；
//   · ComputeNewName —— 对外唯一入口。
// 不读控件、不碰磁盘、不引用对话框的任何成员。
//
// 说明：正则缓存是文件级静态变量。批量重命名窗口是模态的、同一时间只有一个，
//       所以进程内一份缓存足够；它只影响速度，不影响算出来的名字。
// ============================================================================

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <regex>
#include <string>
#include <string.h>
#include <utility>
#include <vector>

#include "ui/BatchRenameRules.h"

namespace
{

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
// 正则：编译一次缓存起来
//
// 每敲一个字符都要把整张表重算一遍，如果每次都现场编译正则，
// 几百项的时候会明显卡；按"写法变了才重编"缓存一下就没这个问题。
// ----------------------------------------------------------------------------
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

} // namespace

bool IsRenameRegexValid(const std::wstring& pattern)
{
    return EnsureRegexCompiled(pattern);
}

std::wstring ComputeNewName(const RenameRules& rules, std::size_t index,
                            const std::wstring& originalName)
{
    std::wstring stem;
    std::wstring extension;
    SplitFileName(originalName, stem, extension);

    // 0) 先把「扩展名」这个下拉定下来（保留 / 改成指定 / 去掉）
    //    放在查找替换前面，是为了让勾了「含扩展名」的时候，查找看到的是"最终要用的那个扩展名"
    const int extMode = (rules.extMode >= 0 && rules.extMode <= 2) ? rules.extMode : 0;

    switch (extMode)
    {
    case 1:   // 修改：右边框填什么就用什么，没填就是不要扩展名
    {
        std::wstring value = rules.extValue;
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
    const bool includeExt = rules.includeExt;

    std::wstring working = includeExt ? (stem + extension) : stem;

    // 1) 查找替换（普通文字，或按正则处理）
    const std::wstring find = rules.find;
    if (!find.empty())
    {
        // 除了「普通查找」以外都按正则解释（内置预设和自定义正则都算）
        const bool useRegex = (rules.regexPreset != 0);

        const std::wstring replacement = rules.replace;

        // 替换范围：0 = 全部；1 = 从左往右数只换前 N 处；2 = 从右往左数只换后 N 处
        // N 取右边那个 30px 窄框（选「替换全部」时它是灰的，不参与）
        // 越界的值按 0 处理，跟原来的 ComboSelection（下拉没有选中项就是 0）一致
        const int replaceMode =
            (rules.replaceMode >= 0 && rules.replaceMode <= 2) ? rules.replaceMode : 0;

        // 空 / 非法 → 1，至少 1（和原来从编辑框读值时 ReadInteger(..., 1, 1) 一致）
        const long long wantCount = (rules.replaceCount > 0) ? rules.replaceCount : 1;

        if (replaceMode == 0)
        {
            // 替换全部：沿用原来的两条路径（正则一次性全换，普通文字全量替换）
            if (useRegex)
            {
                // 正则写法在预览算之前已经验证过（IsRenameRegexValid）；
                // 这里再确认一次（缓存命中时几乎零开销），写错就保持原名
                if (EnsureRegexCompiled(find))
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
                const bool caseSensitive = rules.caseSensitive;

                working = ReplaceAll(working, find, replacement, caseSensitive);
            }
        }
        else
        {
            // 只换其中几处：先把这个串里所有匹配的位置找出来，再挑"前 N 处"或"后 N 处"。
            // 注意正则的替换文字不是固定的一份（$1 这类反向引用每处都不一样），
            // 所以每找到一处就顺手算出它自己的替换文字。
            std::vector<std::pair<size_t, size_t>> matches;       // (起点, 长度)
            std::vector<std::wstring>              pieces;        // 每处对应要替换进去的文字

            const bool caseSensitive = rules.caseSensitive;

            if (useRegex)
            {
                if (EnsureRegexCompiled(find))
                {
                    try
                    {
                        for (std::wsregex_iterator it(working.begin(), working.end(), g_regexCache.regex),
                             last; it != last; ++it)
                        {
                            const std::wsmatch& m = *it;

                            matches.push_back(std::make_pair(
                                static_cast<size_t>(m.position(0)),
                                static_cast<size_t>(m.length(0))));
                            pieces.push_back(m.format(replacement.c_str()));
                        }
                    }
                    catch (const std::regex_error&)
                    {
                        matches.clear();
                        pieces.clear();   // 反向引用写错：这次就别换了
                    }
                }
            }
            else if (!find.empty())
            {
                size_t i = 0;
                while (i + find.size() <= working.size())
                {
                    const bool matched =
                        caseSensitive
                            ? (::wcsncmp(working.c_str() + i, find.c_str(), find.size()) == 0)
                            : (::_wcsnicmp(working.c_str() + i, find.c_str(), find.size()) == 0);

                    if (matched)
                    {
                        matches.push_back(std::make_pair(i, find.size()));
                        pieces.push_back(replacement);
                        i += find.size();
                    }
                    else
                    {
                        ++i;
                    }
                }
            }

            // 选出要换的那几处（按下标区间 [first, last) 挑）
            const size_t all  = matches.size();
            const size_t want = static_cast<size_t>((wantCount > 0) ? wantCount : 1);

            size_t first = 0;
            size_t last  = all;

            if (replaceMode == 1)
            {
                last = (want < all) ? want : all;          // 从左往右：只换前 N 处
            }
            else
            {
                first = (all > want) ? (all - want) : 0;    // 从右往左：只换后 N 处
            }

            std::wstring rebuilt;
            size_t       cursor = 0;

            for (size_t k = 0; k < all; ++k)
            {
                if (k < first || k >= last)
                {
                    continue;   // 这一处不换，原样留着
                }

                const size_t pos = matches[k].first;
                const size_t len = matches[k].second;

                rebuilt += working.substr(cursor, pos - cursor);
                rebuilt += pieces[k];
                cursor = pos + len;
            }

            rebuilt += working.substr(cursor);
            working = rebuilt;
        }
    }

    // 2) 大小写（作用范围跟查找替换一致）
    // 越界的值按 0（保持原样）处理，跟原来的 ComboSelection 一致
    const int caseMode = (rules.caseMode >= 0 && rules.caseMode <= 3) ? rules.caseMode : 0;

    switch (caseMode)
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
    const bool numberOn = rules.number;

    if (numberOn)
    {
        // 起始 / 步长 / 位数都按原来从编辑框读出来的口径夹一遍：
        // 起始最小 0、步长最小 1、位数夹到 1~kMaxNumberDigits
        const long long start  = (rules.numberStart >= 0) ? rules.numberStart : 0;
        const long long step   = (rules.numberStep >= 1) ? rules.numberStep : 1;
        const int       digits = std::clamp(rules.numberDigits, 1, kMaxNumberDigits);

        const std::wstring numberText =
            FormatNumber(start + step * static_cast<long long>(index), digits);

        const bool replaceOn = rules.numberReplace;

        if (replaceOn)
        {
            // 替换：原文件名整段不要了，新名字就是序号本身；
            // 但「分隔符」仍然按「位置」贴在序号的一侧（分隔符留空就是干净的 01）
            const std::wstring separator = rules.numberSep;

            if (rules.numberPos == 1)
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
            const std::wstring separator = rules.numberSep;

            if (rules.numberPos == 1)
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
    stem = rules.prefix + stem + rules.suffix;

    return stem + extension;
}
