// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：tests/BatchRenameRulesTest.cpp
//
// 批量重命名规则引擎的回归测试（C8-1 之后）。
//
// 做什么：
//   读 tests/data/batch-rename-baseline.csv —— 这份 CSV 是【重构前】的
//   BatchRenameDialog::ComputeNewName 逐条跑出来的（规则填进真实控件、由控件读值），
//   作为"旧行为"的基准；然后用重构后的纯函数 ComputeNewName(RenameRules, ...)
//   按同样的用例重算一遍，逐字比对。任何一处不一致都会列出来并返回非 0。
//
// 用例本身在 tests/RenameCases.h 里（和当初生成基准时共用同一份定义）。
//
// 编译 / 运行（在仓库根目录）：
//   tools\run-tests.ps1
// 或者手工：
//   cl /std:c++17 /utf-8 /EHsc /I src /I tests tests\BatchRenameRulesTest.cpp
//      src\ui\BatchRenameRules.cpp /link user32.lib
//   BatchRenameRulesTest.exe tests\data\batch-rename-baseline.csv
// ============================================================================

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "ui/BatchRenameDialog.h"
#include "RenameCases.h"

namespace
{

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }

    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                            static_cast<int>(text.size()),
                                            nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
    {
        return {};
    }

    std::string out(static_cast<size_t>(bytes), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                          out.data(), bytes, nullptr, nullptr);
    return out;
}

std::vector<std::string> Split(const std::string& line, char separator)
{
    std::vector<std::string> parts;
    size_t start = 0;

    for (;;)
    {
        const size_t pos = line.find(separator, start);
        if (pos == std::string::npos)
        {
            parts.push_back(line.substr(start));
            break;
        }

        parts.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }

    return parts;
}

bool ParseInt(const std::string& text, int& value)
{
    try
    {
        size_t used = 0;
        const int parsed = std::stoi(text, &used);
        if (used != text.size())
        {
            return false;
        }
        value = parsed;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

} // namespace

int main(int argc, char** argv)
{
    const std::string baselinePath =
        (argc >= 2) ? argv[1] : "tests/data/batch-rename-baseline.csv";

    std::ifstream input(baselinePath, std::ios::binary);
    if (!input)
    {
        std::fprintf(stderr, "打不开基准文件：%s\n", baselinePath.c_str());
        return 2;
    }

    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());

    const std::vector<RenameCase>& cases = MakeRenameCases();
    const std::vector<std::wstring>& files = RenameCaseFileNames();

    size_t checked = 0;
    size_t failed = 0;
    std::vector<bool> caseSeen(cases.size(), false);

    size_t position = 0;

    while (position <= content.size())
    {
        const size_t lineEnd = content.find('\n', position);
        std::string line = (lineEnd == std::string::npos)
                               ? content.substr(position)
                               : content.substr(position, lineEnd - position);

        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        position = (lineEnd == std::string::npos) ? content.size() + 1 : lineEnd + 1;

        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        const std::vector<std::string> parts = Split(line, '|');
        if (parts.size() != 5)
        {
            std::fprintf(stderr, "基准文件格式不对（应有 5 列）：%s\n", line.c_str());
            return 2;
        }

        int caseIndex = 0;
        int itemIndex = 0;

        if (!ParseInt(parts[0], caseIndex) || !ParseInt(parts[1], itemIndex) ||
            caseIndex < 0 || caseIndex >= static_cast<int>(cases.size()) ||
            itemIndex < 0 || itemIndex >= static_cast<int>(files.size()))
        {
            std::fprintf(stderr, "基准文件里的下标越界：%s\n", line.c_str());
            return 2;
        }

        caseSeen[static_cast<size_t>(caseIndex)] = true;

        // 原始文件名必须跟用例表一致，否则说明用例被改过、基准已经过期
        if (parts[3] != ToUtf8(files[static_cast<size_t>(itemIndex)]))
        {
            std::fprintf(stderr, "用例表和基准文件对不上：case %d item %d 原始名是 %s\n",
                         caseIndex, itemIndex, parts[3].c_str());
            return 2;
        }

        const std::wstring actual = ComputeNewName(
            cases[static_cast<size_t>(caseIndex)].rules,
            static_cast<size_t>(itemIndex),
            files[static_cast<size_t>(itemIndex)]);

        const std::string actualUtf8 = ToUtf8(actual);

        ++checked;

        if (actualUtf8 != parts[4])
        {
            ++failed;

            if (failed <= 30)
            {
                std::fprintf(stderr,
                             "[不一致] 用例 %d「%s」第 %d 项「%s」\n"
                             "         基准：%s\n         实际：%s\n",
                             caseIndex,
                             ToUtf8(cases[static_cast<size_t>(caseIndex)].label).c_str(),
                             itemIndex, parts[3].c_str(),
                             parts[4].c_str(), actualUtf8.c_str());
            }
        }
    }

    for (size_t i = 0; i < caseSeen.size(); ++i)
    {
        if (!caseSeen[i])
        {
            std::fprintf(stderr, "基准文件缺少用例 %zu「%s」\n", i,
                         ToUtf8(cases[i].label).c_str());
            return 2;
        }
    }

    std::printf("批量重命名规则回归：%zu 条，%zu 条不一致（基准：%s）\n",
                checked, failed, baselinePath.c_str());

    if (failed != 0)
    {
        std::fprintf(stderr, "失败：算出来的名字和重构前不一致\n");
        return 1;
    }

    std::printf("全部通过\n");
    return 0;
}
