// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：tests/ActionRunnerTest.cpp
//
// 快捷动作的纯函数 + 真实启动回归：
//   · 参数引号（空格 / 反斜杠 / 引号）；
//   · %1 / %sel% / %dir% 展开，多选一组/多组；
//   · 场景 + 扩展名匹配；
//   · 用 cmd.exe 真实跑一次，验证带空格的路径能原样传进去。
//
// 编译方式见 tools/run-action-tests.ps1。
// ============================================================================

#include "shell/ActionRunner.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace shell;
using ui::CustomAction;
using ui::MenuScene;

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

void CheckEqual(const std::wstring& actual, const std::wstring& expected,
                const char* what)
{
    if (actual == expected) return;
    Check(false, std::string(what) + "\n  actual:   " + Narrow(actual) +
                 "\n  expected: " + Narrow(expected));
}

std::wstring TempDir()
{
    wchar_t path[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, path);
    const std::wstring dir = std::wstring(path) + L"yexp-action-test";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring ReadFile(const std::wstring& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::wstring();
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return Widen(text);
}

void TestQuoting()
{
    CheckEqual(QuoteArgument(L"abc"), L"abc", "quote plain");
    CheckEqual(QuoteArgument(L"a b"), L"\"a b\"", "quote space");
    CheckEqual(QuoteArgument(L"C:\\dir\\"), L"C:\\dir\\", "quote trailing backslash no space");
    CheckEqual(QuoteArgument(L"C:\\my dir\\"), L"\"C:\\my dir\\\\\"",
               "quote trailing backslash with space");
    CheckEqual(QuoteArgument(L"a\"b"), L"\"a\\\"b\"", "quote embedded quote");
    CheckEqual(QuoteArgument(L""), L"\"\"", "quote empty");
}

void TestExpand()
{
    CustomAction action;
    action.arguments = L"%1";

    ActionContext context;
    context.currentDir = L"C:\\work";
    context.paths = { L"C:\\work\\a b.txt" };

    std::vector<std::wstring> result = ExpandActionArguments(action, context);
    Check(result.size() == 1, "single path -> one launch");
    CheckEqual(result[0], L"\"C:\\work\\a b.txt\"", "expand %1 quoted");

    // 用户自己包了引号也不重复
    action.arguments = L"\"%1\"";
    result = ExpandActionArguments(action, context);
    CheckEqual(result[0], L"\"C:\\work\\a b.txt\"", "expand quoted %1");

    // %dir%
    action.arguments = L"-o%dir%";
    result = ExpandActionArguments(action, context);
    CheckEqual(result[0], L"-oC:\\work", "expand %dir%");

    // 多选 + %sel%：一次启动，多个带引号参数
    action.arguments = L"%sel%";
    context.paths = { L"C:\\a b.txt", L"C:\\c.txt" };
    result = ExpandActionArguments(action, context);
    Check(result.size() == 1, "%sel% -> one launch");
    CheckEqual(result[0], L"\"C:\\a b.txt\" C:\\c.txt", "expand %sel%");

    // 多选 + 只有 %1：每个路径一组
    action.arguments = L"%1";
    result = ExpandActionArguments(action, context);
    Check(result.size() == 2, "%1 with multi -> one launch per path");
    CheckEqual(result[0], L"\"C:\\a b.txt\"", "expand multi first");
    CheckEqual(result[1], L"C:\\c.txt", "expand multi second");

    // 空白区：%1 = 当前目录
    context.paths.clear();
    action.arguments = L"%1";
    result = ExpandActionArguments(action, context);
    CheckEqual(result[0], L"C:\\work", "background %1 = current dir");

    // 路径里的 % 不会被二次展开
    action.arguments = L"%%1";
    context.paths = { L"C:\\a%dir%.txt" };
    result = ExpandActionArguments(action, context);
    CheckEqual(result[0], L"%C:\\a%dir%.txt", "no recursive expansion");
}

void TestMatching()
{
    CustomAction action;
    action.scenes = ui::SceneBitFile | ui::SceneBitFolder;
    action.extensions = { L".zip", L".7z" };

    Check(ActionMatchesPath(action, MenuScene::File, L"C:\\a.ZIP", { L"C:\\a.ZIP" }),
          "match file extension case-insensitive");
    Check(!ActionMatchesPath(action, MenuScene::File, L"C:\\a.txt", { L"C:\\a.txt" }),
          "reject wrong extension");
    Check(ActionMatchesPath(action, MenuScene::Folder, L"C:\\folder", { L"C:\\folder" }),
          "folder ignores extension");
    Check(!ActionMatchesPath(action, MenuScene::Multiple, L"C:\\a.zip",
                             { L"C:\\a.zip", L"C:\\b.txt" }),
          "multiple requires all extensions");

    action.extensions.clear();
    Check(ActionMatchesPath(action, MenuScene::File, L"C:\\a.txt", { L"C:\\a.txt" }),
          "empty extensions = all");

    action.scenes = ui::SceneBitBackground;
    Check(!ActionMatchesPath(action, MenuScene::File, L"C:\\a.txt", { L"C:\\a.txt" }),
          "scene bit respected");
}

void TestRun()
{
    const std::wstring dir = TempDir();
    const std::wstring target = dir + L"\\带 空格 的文件.txt";
    const std::wstring out = dir + L"\\action-out.txt";
    ::DeleteFileW(out.c_str());
    {
        std::ofstream file(target, std::ios::binary);
        file << "hello";
    }

    CustomAction action;
    action.title = L"测试动作";
    action.program = L"%COMSPEC%";
    action.arguments = L"/d /c echo %1 > \"" + out + L"\"";
    action.startIn = L"%dir%";

    ActionContext context;
    context.currentDir = dir;
    context.paths = { target };

    std::wstring error;
    const bool ok = RunCustomAction(action, context, nullptr, &error);
    Check(ok, "run cmd action");
    if (!ok)
    {
        std::printf("  error: %s\n", Narrow(error).c_str());
    }

    ::Sleep(200);
    const std::wstring content = ReadFile(out);
    Check(!content.empty(), "action produced output");
    Check(content.find(L"带 空格 的文件.txt") != std::wstring::npos,
          "action received path with spaces");

    // 程序不存在：返回失败 + 可读原因
    CustomAction missing;
    missing.title = L"不存在";
    missing.program = L"C:\\definitely-not-exists\\nope.exe";
    missing.arguments = L"%1";
    error.clear();
    Check(!RunCustomAction(missing, context, nullptr, &error), "missing program fails");
    Check(!error.empty(), "missing program gives error text");

    ::DeleteFileW(out.c_str());
    ::DeleteFileW(target.c_str());
    ::RemoveDirectoryW(dir.c_str());
}

} // namespace

int main()
{
    TestQuoting();
    TestExpand();
    TestMatching();
    TestRun();

    if (g_failures != 0)
    {
        std::printf("\n%d failure(s)\n", g_failures);
        return 1;
    }

    std::printf("ActionRunnerTest: all passed\n");
    return 0;
}
