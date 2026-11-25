// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ActionRunner.cpp
//
// 快捷动作的参数展开与启动。说明见 ActionRunner.h。
// ============================================================================

#include "shell/ActionRunner.h"

#include "core/Logger.h"

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#include <shlwapi.h>
#include <shellapi.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

namespace shell
{

namespace
{

std::wstring ToLower(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return text;
}

std::wstring ExpandEnv(const std::wstring& text)
{
    if (text.empty()) return text;

    DWORD length = ::ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
    if (length == 0) return text;

    std::wstring result(length, L'\0');
    length = ::ExpandEnvironmentStringsW(text.c_str(), result.data(), length);
    if (length == 0) return text;

    result.resize(length - 1);   // 去掉结尾 '\0'
    return result;
}

std::wstring ReplaceAll(std::wstring text, const std::wstring& from, const std::wstring& to)
{
    if (from.empty()) return text;

    size_t position = 0;
    while ((position = text.find(from, position)) != std::wstring::npos)
    {
        text.replace(position, from.size(), to);
        position += to.size();
    }
    return text;
}

// 把 "%1" / "%sel%" / "%dir%" 这层用户自己加的引号去掉。
// 占位符展开时本来就会加引号，不去掉会变成 ""路径""。
std::wstring StripPlaceholderQuotes(std::wstring text)
{
    for (const wchar_t* token : { L"%1", L"%sel%", L"%dir%" })
    {
        text = ReplaceAll(text, std::wstring(L"\"") + token + L"\"", token);
    }
    return text;
}

std::wstring ExpandTemplate(const std::wstring& rawTemplate,
                            const std::vector<std::wstring>& paths,
                            const std::wstring& currentDir)
{
    const std::wstring text = StripPlaceholderQuotes(rawTemplate);

    const std::wstring firstPath = paths.empty() ? currentDir : paths.front();

    std::wstring allPaths;
    for (size_t i = 0; i < paths.size(); ++i)
    {
        if (i != 0) allPaths += L" ";
        allPaths += QuoteArgument(paths[i]);
    }

    std::wstring result;
    for (size_t i = 0; i < text.size(); )
    {
        if (text[i] == L'%')
        {
            if (text.compare(i, 2, L"%1") == 0)
            {
                result += QuoteArgument(firstPath);
                i += 2;
                continue;
            }
            if (text.compare(i, 5, L"%sel%") == 0)
            {
                result += allPaths;
                i += 5;
                continue;
            }
            if (text.compare(i, 5, L"%dir%") == 0)
            {
                result += QuoteArgument(currentDir);
                i += 5;
                continue;
            }
        }

        result.push_back(text[i]);
        ++i;
    }

    return result;
}

std::wstring ExpandStartIn(const std::wstring& rawStartIn,
                           const std::vector<std::wstring>& paths,
                           const std::wstring& currentDir)
{
    std::wstring startIn = ExpandEnv(rawStartIn);
    const std::wstring firstPath = paths.empty() ? currentDir : paths.front();

    std::wstring allPaths;
    for (size_t i = 0; i < paths.size(); ++i)
    {
        if (i != 0) allPaths += L" ";
        allPaths += paths[i];
    }

    startIn = ReplaceAll(startIn, L"%dir%", currentDir);
    startIn = ReplaceAll(startIn, L"%sel%", allPaths);
    startIn = ReplaceAll(startIn, L"%1", firstPath);
    return startIn;
}

std::wstring ErrorText(DWORD code)
{
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    std::wstring text;
    if (length > 0 && buffer != nullptr)
    {
        text.assign(buffer, length);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n'))
        {
            text.pop_back();
        }
        ::LocalFree(buffer);
    }
    if (text.empty())
    {
        text = L"错误码 " + std::to_wstring(code);
    }
    return text;
}

std::wstring ResolveProgram(std::wstring program, std::wstring& error)
{
    program = ExpandEnv(program);
    if (program.empty())
    {
        error = L"没有填写程序路径";
        return std::wstring();
    }

    // 不带路径的（如 notepad.exe）：按 PATH 找
    if (program.find_first_of(L"\\/") == std::wstring::npos)
    {
        wchar_t found[32768] = {};
        if (::SearchPathW(nullptr, program.c_str(), L".exe", 32768, found, nullptr) == 0)
        {
            error = L"PATH 里找不到程序：" + program;
            return std::wstring();
        }
        program = found;
    }
    else
    {
        wchar_t full[32768] = {};
        if (::GetFullPathNameW(program.c_str(), 32768, full, nullptr) == 0)
        {
            error = L"程序路径无法解析：" + program;
            return std::wstring();
        }
        program = full;
    }

    const DWORD attr = ::GetFileAttributesW(program.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        error = L"程序不存在：" + program;
        return std::wstring();
    }

    return program;
}

bool LaunchCreateProcess(const std::wstring& application,
                         const std::wstring& commandLine,
                         const std::wstring& startIn,
                         DWORD* processId,
                         std::wstring& error)
{
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;

    PROCESS_INFORMATION process = {};

    std::wstring mutableCommand = commandLine;

    const BOOL created = ::CreateProcessW(
        application.c_str(),
        mutableCommand.data(),
        nullptr, nullptr, FALSE, 0, nullptr,
        startIn.empty() ? nullptr : startIn.c_str(),
        &startup, &process);

    if (!created)
    {
        error = L"启动失败（" + ErrorText(::GetLastError()) + L"）：" + application;
        return false;
    }

    if (processId != nullptr) *processId = process.dwProcessId;

    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return true;
}

bool ShellExecuteProgram(const std::wstring& verb,
                         const std::wstring& program,
                         const std::wstring& arguments,
                         const std::wstring& startIn,
                         HWND owner,
                         std::wstring& error)
{
    SHELLEXECUTEINFOW execute = {};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOASYNC;
    execute.hwnd = owner;
    execute.lpVerb = verb.c_str();
    execute.lpFile = program.c_str();
    execute.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
    execute.lpDirectory = startIn.empty() ? nullptr : startIn.c_str();
    execute.nShow = SW_SHOWNORMAL;

    if (!::ShellExecuteExW(&execute))
    {
        error = L"ShellExecute 失败（" + ErrorText(::GetLastError()) + L"）：" + program;
        return false;
    }
    return true;
}

bool LaunchOne(const ui::CustomAction& action,
               const std::wstring& program,
               const std::wstring& arguments,
               const std::wstring& startIn,
               HWND owner,
               std::wstring& error)
{
    const std::wstring extension = ToLower(
        std::wstring(::PathFindExtensionW(program.c_str())));

    std::wstring launchArguments = arguments;
    std::wstring launchProgram = program;

    if (extension == L".ps1")
    {
        std::wstring powershell = ExpandEnv(L"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
        launchArguments = L"-NoProfile -ExecutionPolicy Bypass -File " + QuoteArgument(program);
        if (!arguments.empty()) launchArguments += L" " + arguments;
        launchProgram = powershell;

        if (action.runAsAdmin)
        {
            return ShellExecuteProgram(L"runas", powershell, launchArguments, startIn, owner, error);
        }
        return LaunchCreateProcess(powershell, QuoteArgument(powershell) + L" " + launchArguments,
                                   startIn, nullptr, error);
    }

    if (extension == L".bat" || extension == L".cmd")
    {
        const std::wstring comspec = ExpandEnv(L"%COMSPEC%");
        const std::wstring inner = QuoteArgument(program) +
                                   (arguments.empty() ? L"" : L" " + arguments);
        const std::wstring commandLine = QuoteArgument(comspec) + L" /d /s /c \"" + inner + L"\"";

        if (action.runAsAdmin)
        {
            return ShellExecuteProgram(L"runas", comspec,
                                       L"/d /s /c \"" + inner + L"\"", startIn, owner, error);
        }
        return LaunchCreateProcess(comspec, commandLine, startIn, nullptr, error);
    }

    if (extension == L".exe" || extension.empty())
    {
        const std::wstring commandLine = QuoteArgument(program) +
                                         (arguments.empty() ? L"" : L" " + arguments);

        if (action.runAsAdmin)
        {
            return ShellExecuteProgram(L"runas", program, arguments, startIn, owner, error);
        }

        DWORD processId = 0;
        const bool ok = LaunchCreateProcess(program, commandLine, startIn, &processId, error);
        if (ok)
        {
            Logger::Info(L"快捷动作已启动：" + action.title +
                         L"，PID=" + std::to_wstring(processId));
        }
        return ok;
    }

    // .lnk / 文档 / 其它：交给系统外壳
    const bool ok = ShellExecuteProgram(action.runAsAdmin ? L"runas" : L"open",
                                        program, arguments, startIn, owner, error);
    if (ok)
    {
        Logger::Info(L"快捷动作已启动：" + action.title);
    }
    return ok;
}

} // namespace

// ---------------------------------------------------------------------------
// 引号
// ---------------------------------------------------------------------------

std::wstring QuoteArgument(const std::wstring& argument)
{
    if (argument.empty())
    {
        return L"\"\"";
    }

    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    {
        return argument;
    }

    std::wstring result;
    result.push_back(L'"');

    for (size_t i = 0; ; ++i)
    {
        size_t backslashes = 0;
        while (i < argument.size() && argument[i] == L'\\')
        {
            ++i;
            ++backslashes;
        }

        if (i == argument.size())
        {
            result.append(backslashes * 2, L'\\');
            break;
        }

        if (argument[i] == L'"')
        {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
        }
        else
        {
            result.append(backslashes, L'\\');
            result.push_back(argument[i]);
        }
    }

    result.push_back(L'"');
    return result;
}

// ---------------------------------------------------------------------------
// 参数展开
// ---------------------------------------------------------------------------

std::vector<std::wstring> ExpandActionArguments(const ui::CustomAction& action,
                                                const ActionContext& context)
{
    std::vector<std::wstring> targets = context.paths;
    if (targets.empty())
    {
        targets.push_back(context.currentDir);
    }

    const bool hasSel = action.arguments.find(L"%sel%") != std::wstring::npos;

    std::vector<std::wstring> result;
    if (hasSel)
    {
        result.push_back(ExpandTemplate(action.arguments, targets, context.currentDir));
    }
    else
    {
        for (const std::wstring& target : targets)
        {
            result.push_back(ExpandTemplate(action.arguments, { target }, context.currentDir));
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// 匹配
// ---------------------------------------------------------------------------

bool ActionMatchesPath(const ui::CustomAction& action,
                       ui::MenuScene scene,
                       const std::wstring& primaryPath,
                       const std::vector<std::wstring>& paths)
{
    const unsigned sceneBit = 1u << static_cast<unsigned>(scene);
    if ((action.scenes & sceneBit) == 0)
    {
        return false;
    }

    if (action.extensions.empty())
    {
        return true;
    }

    // 扩展名只对文件和文件多选有意义；文件夹/空白区不看
    if (scene == ui::MenuScene::Folder || scene == ui::MenuScene::Background)
    {
        return true;
    }

    auto matches = [&action](const std::wstring& path)
    {
        std::wstring extension = ::PathFindExtensionW(path.c_str());
        extension = ToLower(extension);
        return std::find(action.extensions.begin(), action.extensions.end(), extension) !=
               action.extensions.end();
    };

    if (scene == ui::MenuScene::File)
    {
        return matches(primaryPath);
    }

    if (paths.empty())
    {
        return matches(primaryPath);
    }

    for (const std::wstring& path : paths)
    {
        if (!matches(path)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 执行
// ---------------------------------------------------------------------------

bool RunCustomAction(const ui::CustomAction& action,
                     const ActionContext& context,
                     HWND owner,
                     std::wstring* error)
{
    if (error != nullptr) error->clear();

    std::wstring resolveError;
    const std::wstring program = ResolveProgram(action.program, resolveError);
    if (program.empty())
    {
        if (error != nullptr) *error = resolveError;
        Logger::Warn(L"快捷动作启动失败：" + action.title + L"；" + resolveError);
        return false;
    }

    std::wstring startIn = ExpandStartIn(action.startIn, context.paths, context.currentDir);
    if (!startIn.empty())
    {
        const DWORD attr = ::GetFileAttributesW(startIn.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            startIn = context.currentDir;
        }
    }
    else
    {
        startIn = context.currentDir;
    }

    const std::vector<std::wstring> argumentSets = ExpandActionArguments(action, context);
    if (argumentSets.empty())
    {
        if (error != nullptr) *error = L"参数展开为空";
        return false;
    }

    for (const std::wstring& arguments : argumentSets)
    {
        std::wstring launchError;
        if (!LaunchOne(action, program, arguments, startIn, owner, launchError))
        {
            if (error != nullptr) *error = launchError;
            Logger::Warn(L"快捷动作启动失败：" + action.title + L"；" + launchError);
            return false;
        }
    }

    return true;
}

} // namespace shell
