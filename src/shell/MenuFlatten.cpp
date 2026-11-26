#include "shell/MenuFlatten.h"

#include <algorithm>
#include <cwctype>

namespace shell
{

namespace
{

std::wstring Trim(const std::wstring& text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::iswspace(text[begin])) ++begin;
    while (end > begin && std::iswspace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

// 菜单文字里 & 是快捷键标记，末尾 ... / … 是“还有下级”的提示；
// 比较名字时都去掉，显示时也去掉。
std::wstring CleanTitle(const std::wstring& text)
{
    std::wstring result;
    result.reserve(text.size());
    for (wchar_t ch : text)
    {
        if (ch != L'&') result.push_back(ch);
    }
    result = Trim(result);
    while (!result.empty() &&
           (result.back() == L'.' || result.back() == L'\x2026'))
    {
        result.pop_back();
    }
    return Trim(result);
}

std::wstring TitleKey(const std::wstring& text)
{
    std::wstring result = CleanTitle(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return result;
}

void CollapseSeparators(std::vector<MenuInventoryItem>& items)
{
    std::vector<MenuInventoryItem> result;
    result.reserve(items.size());

    for (MenuInventoryItem& item : items)
    {
        if (item.separator)
        {
            if (result.empty() || result.back().separator) continue;
            result.push_back(std::move(item));
            continue;
        }

        result.push_back(std::move(item));
    }

    while (!result.empty() && result.back().separator) result.pop_back();
    items = std::move(result);
}

// 把同一层里名字相同的“目录”合并成一个，子项按出现顺序拼起来。
// 只合并有子项的项，叶子不参与。
void MergeSameNameDirectories(std::vector<MenuInventoryItem>& items)
{
    std::vector<MenuInventoryItem> result;
    result.reserve(items.size());

    for (MenuInventoryItem& item : items)
    {
        if (item.separator)
        {
            result.push_back(std::move(item));
            continue;
        }

        if (!item.children.empty())
        {
            const std::wstring key = TitleKey(item.text);
            bool merged = false;
            for (MenuInventoryItem& existing : result)
            {
                if (existing.separator || existing.children.empty()) continue;
                if (TitleKey(existing.text) != key) continue;

                if (!result.empty() && result.back().separator)
                {
                    result.pop_back();
                }
                existing.children.insert(existing.children.end(),
                                         std::make_move_iterator(item.children.begin()),
                                         std::make_move_iterator(item.children.end()));
                merged = true;
                break;
            }
            if (merged) continue;
        }

        result.push_back(std::move(item));
    }

    items = std::move(result);
}

} // namespace

FlattenResult NormalizeAppMenu(std::vector<MenuInventoryItem> items,
                               const std::wstring& displayTitle,
                               const std::wstring& appName,
                               int depth)
{
    FlattenResult result;
    result.title = displayTitle.empty() ? appName : displayTitle;

    MergeSameNameDirectories(items);

    int itemCount = 0;
    size_t onlyFolderIndex = 0;
    bool onlyFolder = false;

    for (size_t i = 0; i < items.size(); ++i)
    {
        if (items[i].separator) continue;
        ++itemCount;
        if (!items[i].children.empty())
        {
            onlyFolderIndex = i;
            onlyFolder = true;
        }
        else
        {
            onlyFolder = false;
        }
    }

    // 只有“唯一一个项目，且这个项目是目录”才拍平。
    // 只要还有叶子或第二个目录，就原样保留。
    if (itemCount == 1 && onlyFolder && depth < 3)
    {
        MenuInventoryItem directory = std::move(items[onlyFolderIndex]);
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(onlyFolderIndex));

        std::vector<MenuInventoryItem> children = std::move(directory.children);
        items.insert(items.begin() + static_cast<std::ptrdiff_t>(onlyFolderIndex),
                     std::make_move_iterator(children.begin()),
                     std::make_move_iterator(children.end()));

        // 父项名字保持别名/应用名，不再拼「1级 · 2级」。
        FlattenResult nested = NormalizeAppMenu(std::move(items), result.title,
                                                appName, depth + 1);
        return nested;
    }

    CollapseSeparators(items);
    result.items = std::move(items);
    return result;
}

} // namespace shell
