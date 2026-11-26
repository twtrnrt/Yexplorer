// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：tests/MenuLayoutTest.cpp
//
// 自定义右键菜单的纯函数回归：
//   · 默认布局和改造前的菜单逐项一致；
//   · 隐藏项后连续/首尾分隔线会被收掉；
//   · 快捷动作整组的占位能正确出现/消失；
//   · JSON 读写、坏配置回退、动作字段清洗。
//
// 编译方式见 tools/run-menu-layout-tests.ps1。
// 不读写 %APPDATA% 的真实配置，全部用临时文件。
// ============================================================================

#include "ui/MenuLayout.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ui;

namespace
{

int g_failures = 0;

void Check(bool ok, const std::string& what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

std::string Narrow(const std::wstring& text)
{
    if (text.empty()) return std::string();
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                             static_cast<int>(text.size()),
                                             nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                          result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring Widen(const std::string& text)
{
    if (text.empty()) return std::wstring();
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                             static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                          result.data(), length);
    return result;
}

std::string Join(const std::vector<std::wstring>& tokens)
{
    std::string result;
    for (size_t i = 0; i < tokens.size(); ++i)
    {
        if (i != 0) result += " | ";
        result += Narrow(tokens[i]);
    }
    return result;
}

std::vector<std::wstring> Order(std::initializer_list<const wchar_t*> items)
{
    std::vector<std::wstring> result;
    for (const wchar_t* item : items) result.push_back(item);
    return result;
}

void Expect(const std::vector<std::wstring>& actual,
            const std::vector<std::wstring>& expected,
            const char* name)
{
    if (actual == expected) return;
    Check(false, std::string(name) + "\n  actual:   " + Join(actual) +
                 "\n  expected: " + Join(expected));
}

unsigned SceneBit(MenuScene scene)
{
    return 1u << static_cast<unsigned>(scene);
}

std::vector<std::wstring> AllAvailable(MenuScene scene)
{
    std::vector<std::wstring> result;
    for (const MenuItemInfo& info : MenuItemCatalog())
    {
        if ((info.scenes & SceneBit(scene)) != 0)
        {
            result.push_back(info.id);
        }
    }
    return result;
}

std::wstring TempPath(const wchar_t* name)
{
    wchar_t dir[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}

MenuLayout Defaults()
{
    MenuLayout layout;
    ApplyDefaultMenuLayout(layout);
    return layout;
}

void TestDefaults()
{
    Expect(DefaultMenuOrder(MenuScene::Background),
           Order({ L"view", L"sort", L"refresh", L"sep",
                   L"paste", L"sep", L"undo", L"redo", L"sep",
                   L"newFolder", L"new", L"sep",
                   L"terminal", L"sep", L"actions", L"sep", L"properties" }),
           "background default order");

    Expect(DefaultMenuOrder(MenuScene::File),
           Order({ L"open", L"openInNewWindow", L"openWith", L"sep",
                   L"cut", L"copy", L"copyPath", L"sep",
                   L"undo", L"redo", L"sep", L"delete", L"sep",
                   L"rename", L"sep", L"actions", L"sep", L"properties" }),
           "file default order");

    Expect(DefaultMenuOrder(MenuScene::Folder),
           Order({ L"open", L"openInNewWindow", L"sep",
                   L"cut", L"copy", L"copyPath", L"sep",
                   L"undo", L"redo", L"sep", L"delete", L"sep",
                   L"rename", L"sep", L"terminal", L"sep",
                   L"actions", L"sep", L"properties" }),
           "folder default order");

    Expect(DefaultMenuOrder(MenuScene::Multiple),
           Order({ L"open", L"sep", L"cut", L"copy", L"copyPath", L"sep",
                   L"undo", L"redo", L"sep", L"delete", L"sep",
                   L"batchRename", L"sep", L"actions", L"sep", L"properties" }),
           "multiple default order");
}

void TestResolve()
{
    const MenuLayout layout = Defaults();

    // 全部可用、没有动作：actions 整组消失，前后两条 sep 折叠成一条
    {
        const std::vector<std::wstring> available = AllAvailable(MenuScene::Background);
        Expect(ResolveMenuOrder(layout, MenuScene::Background, available, false),
               Order({ L"view", L"sort", L"refresh", L"sep",
                       L"paste", L"sep", L"undo", L"redo", L"sep",
                       L"newFolder", L"new", L"sep",
                       L"terminal", L"sep", L"properties" }),
               "background all available, no actions");
    }

    // 所有带条件的项都不可用：不能留下连续/首尾分隔线
    {
        std::vector<std::wstring> available;
        for (const wchar_t* id : { L"view", L"sort", L"refresh", L"newFolder",
                                   L"new", L"properties" })
        {
            available.push_back(id);
        }

        Expect(ResolveMenuOrder(layout, MenuScene::Background, available, false),
               Order({ L"view", L"sort", L"refresh", L"sep",
                       L"newFolder", L"new", L"sep", L"properties" }),
               "background conditional items missing");
    }

    // 文件：打开方式/在新窗口打开不可用、撤销不可用
    {
        const std::vector<std::wstring> available =
        {
            L"open", L"cut", L"copy", L"copyPath",
            L"delete", L"rename", L"properties",
        };

        Expect(ResolveMenuOrder(layout, MenuScene::File, available, false),
               Order({ L"open", L"sep", L"cut", L"copy", L"copyPath", L"sep",
                       L"delete", L"sep", L"rename", L"sep", L"properties" }),
               "file conditional items missing");
    }

    // 文件夹：主文件夹里屏蔽剪切/删除、无终端
    {
        const std::vector<std::wstring> available =
        {
            L"open", L"openInNewWindow", L"copy", L"copyPath", L"rename", L"properties",
        };

        Expect(ResolveMenuOrder(layout, MenuScene::Folder, available, false),
               Order({ L"open", L"openInNewWindow", L"sep", L"copy", L"copyPath",
                       L"sep", L"rename", L"sep", L"properties" }),
               "folder suppressed cut/delete");
    }

    // 属性被隐藏：末尾不能残留分隔线
    {
        MenuLayout hidden = Defaults();
        hidden.scenes[L"multiple"] = Order({ L"open", L"sep", L"cut", L"copy", L"copyPath" });

        Expect(ResolveMenuOrder(hidden, MenuScene::Multiple,
                                { L"open", L"cut", L"copy", L"copyPath" }, false),
               Order({ L"open", L"sep", L"cut", L"copy", L"copyPath" }),
               "hidden properties, no trailing separator");
    }

    // 动作组出现/消失
    {
        const std::vector<std::wstring> available = AllAvailable(MenuScene::Multiple);

        Expect(ResolveMenuOrder(layout, MenuScene::Multiple, available, true),
               Order({ L"open", L"sep", L"cut", L"copy", L"copyPath", L"sep",
                       L"undo", L"redo", L"sep", L"delete", L"sep",
                       L"batchRename", L"sep", L"actions", L"sep", L"properties" }),
               "multiple with actions");
    }

    // 空清单 = 整个场景都隐藏
    {
        MenuLayout hidden = Defaults();
        hidden.scenes[L"file"] = {};
        Check(ResolveMenuOrder(hidden, MenuScene::File, AllAvailable(MenuScene::File), false).empty(),
              "empty scene order hides everything");
    }
}

void TestJson()
{
    const std::wstring path = TempPath(L"yexp-menu-layout-test.json");
    ::DeleteFileW(path.c_str());

    MenuLayout saved = Defaults();
    saved.scenes[L"file"] = Order({ L"open", L"sep", L"properties" });
    saved.thirdParty[L"file"] = { { L"WPS Office|0", L"WPS Office", 0 },
                                  { L"WPS Office|1", L"WPS Office", 1 } };
    saved.aliases[BuiltinAliasKey(L"open")] = L"打开文件";
    saved.aliases[ThirdPartyAliasKey(L"WPS Office|0")] = L"WPS 经典";

    CustomAction action;
    action.id      = L"a1";
    action.title   = L"用 VS Code 打开";
    action.program = L"C:\\Apps\\Code.exe";
    action.scenes  = SceneBitFile | SceneBitFolder;
    action.extensions = { L".TXT", L"md" };
    action.order   = 20;
    saved.actions.push_back(action);

    Check(SaveMenuLayoutTo(path, saved), "save layout to temp file");

    MenuLayout loaded = Defaults();
    Check(LoadMenuLayoutFrom(path, loaded), "load layout from temp file");

    Expect(loaded.scenes[L"file"], saved.scenes[L"file"], "file order round-trip");
    Check(loaded.thirdParty[L"file"].size() == 2, "third-party selections round-trip");
    if (loaded.thirdParty[L"file"].size() == 2)
    {
        Check(loaded.thirdParty[L"file"][0].key == L"WPS Office|0" &&
              loaded.thirdParty[L"file"][0].source == 0,
              "third-party classic key round-trip");
        Check(loaded.thirdParty[L"file"][1].key == L"WPS Office|1" &&
              loaded.thirdParty[L"file"][1].source == 1,
              "third-party modern key round-trip");
    }
    Check(loaded.actions.size() == 1, "one action round-trip");
    Check(MenuAlias(loaded, BuiltinAliasKey(L"open"), L"打开") == L"打开文件",
          "builtin alias round-trip");
    Check(MenuAlias(loaded, ThirdPartyAliasKey(L"WPS Office|0"), L"WPS Office") == L"WPS 经典",
          "third-party alias round-trip");
    if (loaded.actions.size() == 1)
    {
        const CustomAction& a = loaded.actions.front();
        Check(a.title == L"用 VS Code 打开", "action title round-trip");
        Check(a.program == L"C:\\Apps\\Code.exe", "action program round-trip");
        Check(a.scenes == (SceneBitFile | SceneBitFolder), "action scenes round-trip");
        Check(a.extensions.size() == 2 && a.extensions[0] == L".txt" &&
              a.extensions[1] == L".md", "action extensions normalized");
        Check(!a.runAsAdmin && a.enabled, "action flags round-trip");
    }

    ::DeleteFileW(path.c_str());

    // 坏 JSON：回默认布局，并生成 .bad
    const std::wstring badPath = TempPath(L"yexp-menu-layout-bad.json");
    ::DeleteFileW(badPath.c_str());
    ::DeleteFileW((badPath + L".bad").c_str());
    {
        HANDLE file = ::CreateFileW(badPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            const char* text = "{ this is not json";
            DWORD written = 0;
            ::WriteFile(file, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
            ::CloseHandle(file);
        }
    }

    MenuLayout broken = Defaults();
    Check(!LoadMenuLayoutFrom(badPath, broken), "broken json reports failure");
    Expect(broken.scenes[L"file"], DefaultMenuOrder(MenuScene::File),
           "broken json falls back to defaults");
    Check(::GetFileAttributesW((badPath + L".bad").c_str()) != INVALID_FILE_ATTRIBUTES,
          "broken json keeps a .bad copy");

    ::DeleteFileW(badPath.c_str());
    ::DeleteFileW((badPath + L".bad").c_str());

    // 未知 ID 被丢弃，其余保留
    const std::wstring unknownPath = TempPath(L"yexp-menu-layout-unknown.json");
    ::DeleteFileW(unknownPath.c_str());
    {
        HANDLE file = ::CreateFileW(unknownPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            const char* text =
                "{\"version\":1,\"scenes\":{\"file\":[\"open\",\"bogus\",\"properties\"]}}";
            DWORD written = 0;
            ::WriteFile(file, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
            ::CloseHandle(file);
        }
    }

    MenuLayout unknown;
    LoadMenuLayoutFrom(unknownPath, unknown);
    Expect(unknown.scenes[L"file"], Order({ L"open", L"properties" }),
           "unknown id dropped, known ids kept");
    ::DeleteFileW(unknownPath.c_str());
}

void TestSeparators()
{
    const std::wstring path = TempPath(L"yexp-menu-layout-sep.json");
    ::DeleteFileW(path.c_str());

    MenuLayout layout = Defaults();
    layout.scenes[L"file"] = Order({ L"open", L"sep", L"copy", L"sep", L"properties" });

    Check(SaveMenuLayoutTo(path, layout), "save multiple separators");
    MenuLayout loaded = Defaults();
    Check(LoadMenuLayoutFrom(path, loaded), "load multiple separators");
    Expect(loaded.scenes[L"file"], layout.scenes[L"file"],
           "multiple separators round-trip (not deduped on load)");

    ::DeleteFileW(path.c_str());
}

void TestThirdPartySeparator()
{
    const std::wstring path = TempPath(L"yexp-menu-layout-third-sep.json");
    ::DeleteFileW(path.c_str());

    MenuLayout layout = Defaults();
    layout.thirdParty[L"file"] = {
        { L"WPS Office|0", L"WPS Office", 0 },
        { L"sep", L"", 0 },
        { L"WPS Office|1", L"WPS Office", 1 }
    };

    Check(SaveMenuLayoutTo(path, layout), "save third-party separator");
    MenuLayout loaded = Defaults();
    Check(LoadMenuLayoutFrom(path, loaded), "load third-party separator");
    Check(loaded.thirdParty[L"file"].size() == 3, "third-party separator round-trip size");
    if (loaded.thirdParty[L"file"].size() == 3)
    {
        Check(loaded.thirdParty[L"file"][1].key == L"sep",
              "third-party separator key round-trip");
    }

    ::DeleteFileW(path.c_str());
}

} // namespace

int main()
{
    TestDefaults();
    TestResolve();
    TestJson();
    TestSeparators();
    TestThirdPartySeparator();

    if (g_failures != 0)
    {
        std::printf("\n%d failure(s)\n", g_failures);
        return 1;
    }

    std::printf("MenuLayoutTest: all passed\n");
    return 0;
}
