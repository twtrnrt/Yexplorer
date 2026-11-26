// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：tests/MenuFlattenTest.cpp
//
// 第三方菜单树拍平/合并的纯函数回归：
//   · 同名包装目录去掉；
//   · 同名目录先合并子项；
//   · 数目录不数叶子（TortoiseSVN：3 个快捷项 + 1 个同名目录仍要拍平）；
//   · 只剩一个不同名目录 → 「1级 · 2级」；
//   · 多个不同目录保留。
//
// 编译方式见 tools/run-flatten-tests.ps1。
// ============================================================================

#include "shell/MenuFlatten.h"

#include <cstdio>
#include <string>
#include <vector>

using shell::FlattenResult;
using shell::MenuInventoryItem;

namespace
{

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

MenuInventoryItem Leaf(const wchar_t* text)
{
    MenuInventoryItem item;
    item.text = text;
    return item;
}

MenuInventoryItem Directory(const wchar_t* text,
                            std::initializer_list<const wchar_t*> children)
{
    MenuInventoryItem item;
    item.text = text;
    for (const wchar_t* child : children) item.children.push_back(Leaf(child));
    return item;
}

bool IsText(const std::vector<MenuInventoryItem>& items, size_t index,
            const wchar_t* text)
{
    return index < items.size() && items[index].text == text;
}

void TestSameNameWrapperOnly()
{
    std::vector<MenuInventoryItem> items = {
        Directory(L"7-Zip", { L"解压到...", L"添加到压缩包..." })
    };

    FlattenResult result = shell::NormalizeAppMenu(std::move(items), L"7-Zip", L"7-Zip");
    Check(result.title == L"7-Zip", "same-name wrapper keeps title");
    Check(result.items.size() == 2, "same-name wrapper removed");
    Check(IsText(result.items, 0, L"解压到...") &&
          IsText(result.items, 1, L"添加到压缩包..."),
          "same-name wrapper children moved up");
}

void TestSameNameWrapperWithLeaves()
{
    // TortoiseSVN：3 个叶子快捷项 + 一个同名目录。
    // 目录数=1，即使还有叶子，也要按“只剩一个目录”拍平。
    std::vector<MenuInventoryItem> items = {
        Leaf(L"SVN 更新"),
        Leaf(L"SVN 提交"),
        Leaf(L"SVN 还原"),
        Directory(L"TortoiseSVN", { L"A", L"B" })
    };

    FlattenResult result = shell::NormalizeAppMenu(
        std::move(items), L"TortoiseSVN", L"TortoiseSVN");
    Check(result.title == L"TortoiseSVN", "leaves + same-name wrapper keeps title");
    Check(result.items.size() == 5, "leaves kept while wrapper flattened");
    Check(IsText(result.items, 0, L"SVN 更新") &&
          IsText(result.items, 3, L"A") &&
          IsText(result.items, 4, L"B"),
          "leaves and wrapper children order");
}

void TestMergeSameNameDirectories()
{
    std::vector<MenuInventoryItem> items = {
        Directory(L"WinRAR", { L"A" }),
        Directory(L"WinRAR", { L"B" }),
        Leaf(L"C")
    };

    FlattenResult result = shell::NormalizeAppMenu(
        std::move(items), L"WinRAR", L"WinRAR");
    Check(result.title == L"WinRAR", "merged same-name dirs keep title");
    Check(result.items.size() == 3, "merged same-name dirs then flattened");
    Check(IsText(result.items, 0, L"A") && IsText(result.items, 1, L"B") &&
          IsText(result.items, 2, L"C"),
          "merged children order");
}

void TestDifferentNameOnlyDirectory()
{
    std::vector<MenuInventoryItem> items = {
        Leaf(L"快捷"),
        Directory(L"转为PDF", { L"转为PDF", L"转为图片" })
    };

    FlattenResult result = shell::NormalizeAppMenu(
        std::move(items), L"WPS Office", L"WPS Office");
    Check(result.title == L"WPS Office · 转为PDF", "different-name dir merges titles");
    Check(result.items.size() == 3, "different-name dir flattened with leaves");
    Check(IsText(result.items, 0, L"快捷") &&
          IsText(result.items, 1, L"转为PDF") &&
          IsText(result.items, 2, L"转为图片"),
          "different-name dir children moved up");
}

void TestMultipleDirectoriesKept()
{
    std::vector<MenuInventoryItem> items = {
        Directory(L"转为PDF", { L"A" }),
        Directory(L"WPS AI", { L"B" }),
        Leaf(L"C")
    };

    FlattenResult result = shell::NormalizeAppMenu(
        std::move(items), L"WPS Office", L"WPS Office");
    Check(result.title == L"WPS Office", "multiple dirs keep title");
    Check(result.items.size() == 3, "multiple dirs counted as leaves");
    Check(!result.items[0].children.empty() && !result.items[1].children.empty(),
          "multiple different dirs kept");
}

void TestAliasMatchesOriginalAppName()
{
    std::vector<MenuInventoryItem> items = {
        Directory(L"WPS Office", { L"A" })
    };

    FlattenResult result = shell::NormalizeAppMenu(
        std::move(items), L"WPS 经典", L"WPS Office");
    Check(result.title == L"WPS 经典", "alias kept when wrapper matches original app name");
    Check(result.items.size() == 1 && IsText(result.items, 0, L"A"),
          "alias wrapper flattened");
}

} // namespace

int main()
{
    TestSameNameWrapperOnly();
    TestSameNameWrapperWithLeaves();
    TestMergeSameNameDirectories();
    TestDifferentNameOnlyDirectory();
    TestMultipleDirectoriesKept();
    TestAliasMatchesOriginalAppName();

    if (g_failures != 0)
    {
        std::printf("\n%d failure(s)\n", g_failures);
        return 1;
    }

    std::printf("MenuFlattenTest: all passed\n");
    return 0;
}
