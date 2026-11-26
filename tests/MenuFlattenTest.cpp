// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：tests/MenuFlattenTest.cpp
//
// 第三方菜单树拍平/合并的纯函数回归：
//   · 同名目录先合并子项；
//   · 只有2级唯一一个目录、没有其他项目时才拍平；
//   · 有叶子/多个项目不拍平（TortoiseSVN：3 个快捷项 + 1 个目录保留）；
//   · 拍平后父项始终用别名/应用名，不拼「1级 · 2级」；
//   · 拍平后子项若又唯一目录，继续拍。
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

MenuInventoryItem Directory(const wchar_t* text,
                            std::initializer_list<MenuInventoryItem> children)
{
    MenuInventoryItem item;
    item.text = text;
    for (const MenuInventoryItem& child : children) item.children.push_back(child);
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

void TestMultipleItemsNotFlattened()
{
    // TortoiseSVN：3 个叶子快捷项 + 一个同名目录 = 4 个项目。
    // 有叶子就不再是“唯一一个目录”，不拍平。
    std::vector<MenuInventoryItem> items = {
        Leaf(L"SVN 更新"),
        Leaf(L"SVN 提交"),
        Leaf(L"SVN 还原"),
        Directory(L"TortoiseSVN", { L"A", L"B" })
    };

    FlattenResult result = shell::NormalizeAppMenu(
        std::move(items), L"TortoiseSVN", L"TortoiseSVN");
    Check(result.title == L"TortoiseSVN", "multiple items keep title");
    Check(result.items.size() == 4, "multiple items not flattened");
    Check(IsText(result.items, 0, L"SVN 更新") &&
          IsText(result.items, 3, L"TortoiseSVN") &&
          result.items[3].children.size() == 2,
          "leaves and wrapper kept when more than one item");
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
    Check(result.items.size() == 2, "merged same-name dirs not flattened with leaf");
    Check(result.items[0].text == L"WinRAR" &&
          result.items[0].children.size() == 2 &&
          IsText(result.items[0].children, 0, L"A") &&
          IsText(result.items[0].children, 1, L"B") &&
          IsText(result.items, 1, L"C"),
          "merged children order, leaf kept");
}

void TestMergedSameNameOnlyFlattened()
{
    std::vector<MenuInventoryItem> items = {
        Directory(L"WinRAR", { L"A" }),
        Directory(L"WinRAR", { L"B" })
    };

    FlattenResult result = shell::NormalizeAppMenu(
        std::move(items), L"WinRAR", L"WinRAR");
    Check(result.title == L"WinRAR", "merged single dir keeps title");
    Check(result.items.size() == 2, "merged single dir flattened");
    Check(IsText(result.items, 0, L"A") && IsText(result.items, 1, L"B"),
          "merged single dir children moved up");
}

void TestDifferentNameWithLeafNotFlattened()
{
    std::vector<MenuInventoryItem> items = {
        Leaf(L"快捷"),
        Directory(L"转为PDF", { L"转为PDF", L"转为图片" })
    };

    FlattenResult result = shell::NormalizeAppMenu(
        std::move(items), L"WPS Office", L"WPS Office");
    Check(result.title == L"WPS Office", "leaf + different-name dir keeps title");
    Check(result.items.size() == 2, "leaf + different-name dir not flattened");
    Check(IsText(result.items, 0, L"快捷") &&
          result.items[1].text == L"转为PDF" &&
          result.items[1].children.size() == 2,
          "leaf and different-name dir kept");
}

void TestDifferentNameSoleFolderFlattened()
{
    std::vector<MenuInventoryItem> items = {
        Directory(L"转为PDF", { L"转为PDF", L"转为图片" })
    };

    FlattenResult result = shell::NormalizeAppMenu(
        std::move(items), L"WPS 经典", L"WPS Office");
    Check(result.title == L"WPS 经典", "sole different-name folder keeps alias title");
    Check(result.items.size() == 2, "sole different-name folder flattened");
    Check(IsText(result.items, 0, L"转为PDF") &&
          IsText(result.items, 1, L"转为图片"),
          "sole different-name folder children moved up");
}

void TestRecursiveSoleFolders()
{
    std::vector<MenuInventoryItem> items = {
        Directory(L"外层", { Directory(L"内层", { L"L" }) })
    };

    FlattenResult result = shell::NormalizeAppMenu(
        std::move(items), L"应用", L"应用");
    Check(result.title == L"应用", "recursive sole folders keep alias title");
    Check(result.items.size() == 1 && IsText(result.items, 0, L"L"),
          "recursive sole folders flattened");
}

void TestSoleFolderWithMultipleChildrenStops()
{
    std::vector<MenuInventoryItem> items = {
        Directory(L"外层", { Leaf(L"叶子"), Directory(L"内层", { L"L" }) })
    };

    FlattenResult result = shell::NormalizeAppMenu(
        std::move(items), L"应用", L"应用");
    Check(result.title == L"应用", "sole folder with multiple children keeps title");
    Check(result.items.size() == 2, "sole folder with multiple children flattened once");
    Check(IsText(result.items, 0, L"叶子") &&
          result.items[1].text == L"内层" &&
          result.items[1].children.size() == 1,
          "inner folder kept when sibling items exist");
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
    TestMultipleItemsNotFlattened();
    TestMergeSameNameDirectories();
    TestMergedSameNameOnlyFlattened();
    TestDifferentNameWithLeafNotFlattened();
    TestDifferentNameSoleFolderFlattened();
    TestRecursiveSoleFolders();
    TestSoleFolderWithMultipleChildrenStops();
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
