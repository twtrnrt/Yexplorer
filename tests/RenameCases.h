// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：tests/RenameCases.h
//
// 批量重命名规则引擎的回归用例（改名前 -> 改名后 的对照表）。
//
// 为什么单独放在这里：
//   C8-1 把 ComputeNewName 从"直接读界面控件"改成"读 RenameRules 值对象"。
//   这是行为敏感改造，必须能证明改完之后算出来的名字和旧实现逐字相同。
//   所以用例在这里只定义一次（规则 + 原始文件名），
//   由两个程序共用：
//     · 重构前：tests-tmp 里的临时程序把规则填进真实控件，跑旧实现，
//       生成 tests/data/batch-rename-baseline.csv（已入库，作为行为基准）；
//     · 重构后：tests/BatchRenameRulesTest.cpp 读同一份 CSV 逐条核对。
//
// 注意：这里只做纯内存计算，不碰磁盘；文件名可以有 Windows 实际建不出来的
//       写法（比如结尾的点），那是为了覆盖 SplitFileName / 扩展名处理的分支。
// ============================================================================
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "ui/BatchRenameDialog.h"

// 一条用例：一套规则 + 一个可读的名字
struct RenameCase
{
    std::wstring   label;
    RenameTemplate rules;
};

// ----------------------------------------------------------------------------
// 用例里使用的"原始文件名"。
//
// 按顺序模拟排序后的第 0、1、2… 项：第 i 个文件对应的序号就是 i。
// 选这些名字是为了覆盖：普通名字、多扩展名、没有扩展名、点开头的隐藏文件、
// 中文/空格、大小写混合、正则特殊字符、结尾是点。
// ----------------------------------------------------------------------------
inline const std::vector<std::wstring>& RenameCaseFileNames()
{
    static const std::vector<std::wstring> names =
    {
        L"alpha.txt",
        L"Beta.TXT",
        L".gitignore",
        L"报告 2024.docx",
        L"a.b.c.txt",
        L"noext",
        L"trailing.",
        L"100% (copy).txt",
        L"MiXeD case Name.md",
        L"file12name.txt"
    };

    return names;
}

// ----------------------------------------------------------------------------
// 全部用例。默认值 = 界面上"什么都没设"。
// ----------------------------------------------------------------------------
inline std::vector<RenameCase> MakeRenameCases()
{
    std::vector<RenameCase> cases;

    auto add = [&cases](const wchar_t* label, const std::function<void(RenameTemplate&)>& setup)
    {
        RenameCase item;
        item.label = label;
        setup(item.rules);
        cases.push_back(std::move(item));
    };

    // ---- 基线 / 前后缀 ----
    add(L"默认（所有规则都是默认值）", [](RenameTemplate&) {});

    add(L"前缀 P_",
        [](RenameTemplate& t) { t.prefix = L"P_"; });

    add(L"后缀 _done",
        [](RenameTemplate& t) { t.suffix = L"_done"; });

    add(L"前后缀同时",
        [](RenameTemplate& t) { t.prefix = L"[["; t.suffix = L"]]"; });

    add(L"前缀带空格",
        [](RenameTemplate& t) { t.prefix = L"  "; });

    // ---- 普通查找替换 ----
    add(L"查找 a 替换 X（默认不区分大小写）",
        [](RenameTemplate& t) { t.find = L"a"; t.replace = L"X"; });

    add(L"查找 a 替换 X（区分大小写）",
        [](RenameTemplate& t) { t.find = L"a"; t.replace = L"X"; t.caseSensitive = true; });

    add(L"查找 A 替换 X（区分大小写，命中大写 A）",
        [](RenameTemplate& t) { t.find = L"A"; t.replace = L"X"; t.caseSensitive = true; });

    add(L"删除所有空格（含扩展名）",
        [](RenameTemplate& t) { t.find = L" "; t.replace = L""; t.includeExt = true; });

    add(L"查找 . 替换 _（含扩展名）",
        [](RenameTemplate& t) { t.find = L"."; t.replace = L"_"; t.includeExt = true; });

    add(L"删除所有点（含扩展名）",
        [](RenameTemplate& t) { t.find = L"."; t.replace = L""; t.includeExt = true; });

    // ---- 替换范围 / 次数 ----
    add(L"替换前 1 处（从左往右）",
        [](RenameTemplate& t) { t.find = L"a"; t.replace = L"X"; t.replaceMode = 1; t.replaceCount = 1; });

    add(L"替换前 2 处（含扩展名）",
        [](RenameTemplate& t)
        {
            t.find = L"."; t.replace = L"_"; t.includeExt = true;
            t.replaceMode = 1; t.replaceCount = 2;
        });

    add(L"替换后 1 处（从右往左）",
        [](RenameTemplate& t)
        {
            t.find = L" "; t.replace = L"-";
            t.replaceMode = 2; t.replaceCount = 1;
        });

    // ---- 正则 ----
    add(L"正则：数字加方括号",
        [](RenameTemplate& t)
        {
            t.find = L"([0-9]+)"; t.replace = L"[$1]"; t.regexPreset = 10;
        });

    add(L"正则：字母数字换位（含扩展名）",
        [](RenameTemplate& t)
        {
            t.find = L"([a-z])([0-9])"; t.replace = L"$2$1"; t.regexPreset = 10;
            t.includeExt = true;
        });

    add(L"正则：写法错误",
        [](RenameTemplate& t) { t.find = L"("; t.replace = L"X"; t.regexPreset = 10; });

    add(L"正则：从右往左替换 1 处",
        [](RenameTemplate& t)
        {
            t.find = L"[aeiou]"; t.replace = L"*"; t.regexPreset = 10;
            t.replaceMode = 2; t.replaceCount = 1;
        });

    // ---- 序号 ----
    add(L"序号：起始 5 步长 3 四位，放前面",
        [](RenameTemplate& t)
        {
            t.number = true; t.numberStart = 5; t.numberStep = 3;
            t.numberDigits = 4; t.numberPos = 1; t.numberSep = L"-";
        });

    add(L"序号：放后面，无分隔符",
        [](RenameTemplate& t)
        {
            t.number = true; t.numberPos = 0; t.numberSep = L"";
        });

    add(L"序号：替换原文件名（放前面，分隔符 _）",
        [](RenameTemplate& t)
        {
            t.number = true; t.numberReplace = true;
            t.numberPos = 1; t.numberSep = L"_";
        });

    add(L"序号：起始 0 一位",
        [](RenameTemplate& t) { t.number = true; t.numberStart = 0; t.numberDigits = 1; });

    add(L"序号：起始 99 两位（超过位数不截断）",
        [](RenameTemplate& t) { t.number = true; t.numberStart = 99; t.numberDigits = 2; });

    add(L"序号：位数 0（按 1 位处理）",
        [](RenameTemplate& t) { t.number = true; t.numberDigits = 0; });

    add(L"序号 + 含扩展名 + 查找点",
        [](RenameTemplate& t)
        {
            t.number = true; t.includeExt = true;
            t.find = L"."; t.replace = L"_";
        });

    // ---- 扩展名 ----
    add(L"扩展名改成 md",
        [](RenameTemplate& t) { t.extMode = 1; t.extValue = L"md"; });

    add(L"扩展名改成 .log（带点）",
        [](RenameTemplate& t) { t.extMode = 1; t.extValue = L".log"; });

    add(L"扩展名清空（去掉扩展名）",
        [](RenameTemplate& t) { t.extMode = 1; t.extValue = L""; });

    add(L"扩展名模式 2（老模板的去掉扩展名）",
        [](RenameTemplate& t) { t.extMode = 2; });

    // ---- 大小写 ----
    add(L"全部大写",
        [](RenameTemplate& t) { t.caseMode = 1; });

    add(L"全部小写",
        [](RenameTemplate& t) { t.caseMode = 2; });

    add(L"首字母大写",
        [](RenameTemplate& t) { t.caseMode = 3; });

    // ---- 组合 ----
    add(L"组合：前缀 + 序号 + 大写 + 改扩展名",
        [](RenameTemplate& t)
        {
            t.prefix = L"P_"; t.suffix = L"_S";
            t.number = true; t.numberDigits = 3; t.numberSep = L"#";
            t.caseMode = 1; t.extMode = 1; t.extValue = L"dat";
        });

    add(L"组合：查找替换 + 大小写 + 含扩展名",
        [](RenameTemplate& t)
        {
            t.find = L"e"; t.replace = L"3"; t.includeExt = true; t.caseMode = 2;
        });

    // ---- 越界 / 非法输入：界面上会被夹到合法范围，这里锁住夹取后的行为 ----
    add(L"序号：起始负数按 0",
        [](RenameTemplate& t) { t.number = true; t.numberStart = -3; });

    add(L"序号：步长 0 按 1",
        [](RenameTemplate& t) { t.number = true; t.numberStep = 0; });

    add(L"序号：位数负数按 1",
        [](RenameTemplate& t) { t.number = true; t.numberDigits = -5; });

    add(L"序号：位数 99 夹到 20",
        [](RenameTemplate& t) { t.number = true; t.numberDigits = 99; });

    add(L"替换：次数 0 按 1（从左往右）",
        [](RenameTemplate& t)
        {
            t.find = L"a"; t.replace = L"X";
            t.replaceMode = 1; t.replaceCount = 0;
        });

    add(L"替换：次数负数按 1（从右往左）",
        [](RenameTemplate& t)
        {
            t.find = L"a"; t.replace = L"X";
            t.replaceMode = 2; t.replaceCount = -2;
        });

    return cases;
}
