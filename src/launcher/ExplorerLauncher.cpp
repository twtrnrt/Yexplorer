// ============================================================================
//  ExplorerLauncher —— explorer.exe 的“薄代理”
//  文件：src/launcher/ExplorerLauncher.cpp
//
//  【它是什么】
//    一个很小的 exe，编译出来的文件名必须是 explorer.exe。
//    把它分别复制到 C:\Windows\System32\explorer.exe（64 位版）
//    和 C:\Windows\SysWOW64\explorer.exe（32 位版），
//    那些“用相对名启动 explorer.exe”的第三方程序（微信、批量打印软件等）
//    就会先命中它，而不是系统的 C:\Windows\explorer.exe。
//    （系统查找可执行文件时，System32 排在 Windows 目录之前）
//
//  【它做什么】
//    只做一件事：看参数，分流。
//      1) /select,"路径"（打开所在文件夹并选中文件）  → 交给本文件管理器
//      2) 盘符路径 / UNC 路径（打开这个文件夹）        → 交给本文件管理器
//      3) 其它（无参数、/e、/n、/root、shell:xxx、::CLSID 等）→ 原样交给真正的
//         C:\Windows\explorer.exe
//    第 3 条是安全阀：任何看不懂的调用都直通系统，保证“不会因为我们而打不开东西”。
//
//  【它不做什么】
//    - 不弹任何界面、不写任何注册表、不常驻。
//    - 不改系统文件：只是新增一个同名的“影子文件”，删掉就立刻恢复原样。
//
//  【它怎么找到本文件管理器】
//    读“注册为默认文件管理器”时写下的那条命令：
//      HKCU\Software\Classes\Directory\shell\open\command
//    值形如：  "D:\...\FileExplorer.exe" "%1"
//    取出引号里的 exe 全路径。
//    - 读不到、或取到的就是 explorer.exe 本身（说明用户已经恢复系统默认），
//      一律直通系统 explorer，绝不递归调用自己。
//    - 取到的路径不存在（程序被删/被挪），同样直通系统 explorer。
//
//  【为什么不会死循环】
//    直通系统时用的是完整路径 C:\Windows\explorer.exe，
//    System32 里的同名影子文件不会被命中。
// ============================================================================

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace {

// 日志文件最多留这么大，超了就从空文件重新写，避免无限增长
const DWORD kLogMaxBytes = 256 * 1024;

// 取一个便于人读的时间戳，形如 2026-11-16 15:04:05.123
std::wstring NowStamp()
{
    SYSTEMTIME st{};
    ::GetLocalTime(&st);

    wchar_t buf[64]{};
    ::swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
                 st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

// 日志目录：优先 %LOCALAPPDATA%\Yexplorer，其次 %TEMP%\Yexp，最后 C:\Windows\Temp
std::wstring LogFilePath()
{
    std::wstring dir;

    wchar_t env[512]{};
    if (::GetEnvironmentVariableW(L"LOCALAPPDATA", env, 512) > 0 && env[0] != L'\0')
    {
        dir = std::wstring(env) + L"\\Yexplorer";
    }
    else if (::GetEnvironmentVariableW(L"TEMP", env, 512) > 0 && env[0] != L'\0')
    {
        // 兜底日志也收进统一的临时根目录 %TEMP%\Yexp
        dir = std::wstring(env) + L"\\Yexp";
    }
    else
    {
        dir = L"C:\\Windows\\Temp";
    }

    // 目录不存在就建一个（已存在会返回 ERROR_ALREADY_EXISTS，忽略即可）
    ::CreateDirectoryW(dir.c_str(), nullptr);

    return dir + L"\\launcher.log";
}

// 追加一行日志。写失败不影响主流程——这个代理的第一职责是“别让用户打不开东西”。
void AppendLog(const std::wstring& line)
{
    const std::wstring path = LogFilePath();

    // 文件太大就清空重来
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr))
    {
        const ULONGLONG size = static_cast<ULONGLONG>(attr.nFileSizeHigh) << 32 | attr.nFileSizeLow;
        if (size > kLogMaxBytes)
        {
            ::DeleteFileW(path.c_str());
        }
    }

    HANDLE file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }

    const std::wstring text = NowStamp() + L"  " + line + L"\r\n";

    // 日志统一用 UTF-8 字节写，记事本/VS Code 都能正常看中文
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                            nullptr, 0, nullptr, nullptr);
    if (bytes > 0)
    {
        std::vector<char> utf8(static_cast<size_t>(bytes));
        ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                              utf8.data(), bytes, nullptr, nullptr);
        DWORD written = 0;
        ::WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }

    ::CloseHandle(file);
}

// 跳过命令行里的第一个 token（也就是 argv[0]，程序路径本身），返回它后面的原始参数串。
// 直通系统 explorer 时要用“原封不动的参数”，不能自己重新拼，否则
// /select,"C:\a b\c.txt" 这种带空格的写法会被改坏。
const wchar_t* SkipFirstToken(const wchar_t* cmdLine)
{
    if (cmdLine == nullptr)
    {
        return L"";
    }

    const wchar_t* p = cmdLine;
    while (*p == L' ' || *p == L'\t')
    {
        ++p;
    }

    if (*p == L'"')
    {
        // 带引号：跳到下一个引号之后
        ++p;
        while (*p != L'\0' && *p != L'"')
        {
            ++p;
        }
        if (*p == L'"')
        {
            ++p;
        }
    }
    else
    {
        // 不带引号：跳到第一个空白
        while (*p != L'\0' && *p != L' ' && *p != L'\t')
        {
            ++p;
        }
    }

    while (*p == L' ' || *p == L'\t')
    {
        ++p;
    }
    return p;
}

// 从一条命令行里取出第一个 token 的路径（用于解析注册表里那条 "exe" "%1"）
std::wstring FirstTokenPath(const std::wstring& command)
{
    const wchar_t* p = command.c_str();
    while (*p == L' ' || *p == L'\t')
    {
        ++p;
    }

    std::wstring result;
    if (*p == L'"')
    {
        ++p;
        while (*p != L'\0' && *p != L'"')
        {
            result.push_back(*p);
            ++p;
        }
    }
    else
    {
        while (*p != L'\0' && *p != L' ' && *p != L'\t')
        {
            result.push_back(*p);
            ++p;
        }
    }

    // 展开 %SystemRoot% 这类环境变量（恢复系统默认时写的就是 %SystemRoot%\explorer.exe）
    if (!result.empty())
    {
        wchar_t expanded[MAX_PATH * 2]{};
        if (::ExpandEnvironmentStringsW(result.c_str(), expanded, MAX_PATH * 2) > 0)
        {
            result = expanded;
        }
    }
    return result;
}

// 去掉路径首尾可能残留的引号：有的程序（以及命令行测试脚本）会把引号、
// 甚至 \" 这种转义引号当普通字符带进参数里，不清理就会变成“路径不存在”。
std::wstring TrimPathQuotes(const std::wstring& s)
{
    std::wstring r = s;

    // 首部：\" 或 "
    if (r.size() >= 2 && r[0] == L'\\' && r[1] == L'"')
    {
        r.erase(0, 2);
    }
    else if (!r.empty() && r[0] == L'"')
    {
        r.erase(0, 1);
    }

    // 尾部：\" 或 "
    if (r.size() >= 2 && r[r.size() - 2] == L'\\' && r[r.size() - 1] == L'"')
    {
        r.erase(r.size() - 2, 2);
    }
    else if (!r.empty() && r[r.size() - 1] == L'"')
    {
        r.erase(r.size() - 1, 1);
    }

    // 去掉末尾多余空白（不含路径本身的空格）
    while (!r.empty() && (r.back() == L' ' || r.back() == L'\t'))
    {
        r.pop_back();
    }
    return r;
}

bool FileExists(const std::wstring& path)
{
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// 取文件名部分（只比较名字，用来判断“这是不是 explorer.exe 本身”）
std::wstring FileNameOf(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// 真正的系统资源管理器：C:\Windows\explorer.exe（用 Windows 目录拼，别用 System32！）
std::wstring RealExplorerPath()
{
    wchar_t winDir[MAX_PATH]{};
    if (::GetWindowsDirectoryW(winDir, MAX_PATH) == 0)
    {
        return L"C:\\Windows\\explorer.exe";
    }
    return std::wstring(winDir) + L"\\explorer.exe";
}

// 读注册表，拿到“当前默认文件管理器”的 exe 全路径。
// 拿不到、或者拿到的就是 explorer.exe，就返回空串，表示“没有可用的本程序”。
std::wstring ResolveOwnExplorer()
{
    const wchar_t* kKey = L"Software\\Classes\\Directory\\shell\\open\\command";

    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return L"";
    }

    wchar_t buffer[2 * MAX_PATH]{};
    DWORD type = 0;
    DWORD size = sizeof(buffer);
    const LONG rc = ::RegQueryValueExW(key, nullptr, nullptr, &type,
                                      reinterpret_cast<LPBYTE>(buffer), &size);
    ::RegCloseKey(key);

    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
    {
        return L"";
    }

    const std::wstring exe = FirstTokenPath(std::wstring(buffer));
    if (exe.empty() || !FileExists(exe))
    {
        return L"";
    }

    // 防递归：如果指向的是 explorer.exe（比如用户已经恢复系统默认），
    // 绝不能再去启动它——那份影子里可能就是我们自己。
    if (_wcsicmp(FileNameOf(exe).c_str(), L"explorer.exe") == 0)
    {
        return L"";
    }

    return exe;
}

// 是不是一个“本地绝对路径”：X:\... 或 \\服务器\...
// 只认这两种。shell:xxx、::{CLSID}、http: 之类一律不碰，原样交给系统 explorer。
bool IsLocalAbsolutePath(const std::wstring& arg)
{
    if (arg.size() >= 3 && arg[1] == L':' && (arg[2] == L'\\' || arg[2] == L'/'))
    {
        const wchar_t drive = arg[0];
        const bool isLetter = (drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z');
        return isLetter;
    }

    // UNC：双反斜杠开头
    if (arg.size() >= 2 && arg[0] == L'\\' && arg[1] == L'\\')
    {
        return true;
    }

    return false;
}

// 把 file:///C:/... 或 file://server/share/... 转回 Windows 路径。
//
// 为什么需要：Chromium / Electron 系程序（比如 DeepSeek Harness 的“显示文件位置”）
// 调 explorer.exe 时传的是 file URL，不是本地路径：
//     explorer.exe "/select," "file:///C:/git/explorer"
// 以前这里只认 C:\... 和 \\server\share，URL 会被当成“看不懂”直通系统资源管理器。
std::wstring FileUrlToPath(const std::wstring& url)
{
    if (_wcsnicmp(url.c_str(), L"file:", 5) != 0)
    {
        return url;
    }

    std::wstring rest = url.substr(5);

    // 去掉 file:// 这一层
    if (rest.size() >= 2 && rest[0] == L'/' && rest[1] == L'/')
    {
        rest.erase(0, 2);
    }

    // 现在可能是：
    //   /C:/path        （file:///C:/path，三个斜杠留下一个前导 /）
    //   C:/path         （file:C:/path，少见但合法）
    //   server/share    （file://server/share，UNC）
    bool unc = false;
    if (!rest.empty() && rest[0] == L'/')
    {
        rest.erase(0, 1);
    }
    else if (rest.size() >= 2 && rest[1] == L':')
    {
        // 盘符路径，无需额外处理
    }
    else
    {
        unc = true;
    }

    // 百分号解码：URL 里的 %20 是空格、%2C 是逗号（DSH 会把路径里的 , 和 = 编码），
    // 非 ASCII 字符也按 UTF-8 逐字节还原。
    std::string bytes;
    bytes.reserve(rest.size());

    auto hexValue = [](wchar_t c) -> int
    {
        if (c >= L'0' && c <= L'9')
        {
            return c - L'0';
        }
        if (c >= L'a' && c <= L'f')
        {
            return c - L'a' + 10;
        }
        if (c >= L'A' && c <= L'F')
        {
            return c - L'A' + 10;
        }
        return -1;
    };

    for (size_t i = 0; i < rest.size(); ++i)
    {
        if (rest[i] == L'%' && i + 2 < rest.size())
        {
            const int hi = hexValue(rest[i + 1]);
            const int lo = hexValue(rest[i + 2]);
            if (hi >= 0 && lo >= 0)
            {
                bytes.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }

        const wchar_t c = rest[i];
        if (c < 0x80)
        {
            bytes.push_back(static_cast<char>(c));
        }
        else
        {
            char utf8[4]{};
            const int n = ::WideCharToMultiByte(CP_UTF8, 0, &c, 1, utf8, 4, nullptr, nullptr);
            if (n > 0)
            {
                bytes.append(utf8, static_cast<size_t>(n));
            }
        }
    }

    std::wstring path;
    if (!bytes.empty())
    {
        const int len = ::MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(),
                                              static_cast<int>(bytes.size()), nullptr, 0);
        if (len > 0)
        {
            path.resize(static_cast<size_t>(len));
            ::MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), static_cast<int>(bytes.size()),
                                  path.data(), len);
        }
    }

    // URL 用正斜杠，转回 Windows 的反斜杠
    for (wchar_t& c : path)
    {
        if (c == L'/')
        {
            c = L'\\';
        }
    }

    if (unc)
    {
        path = L"\\\\" + path;
    }

    return path;
}

// 启动一个进程（不等待它结束）。目标路径一律传完整路径，防止再次命中自己。
bool Spawn(const std::wstring& exe, const std::wstring& args, DWORD* outPid)
{
    if (!FileExists(exe))
    {
        return false;
    }

    std::wstring cmdLine = L"\"" + exe + L"\"";
    if (!args.empty())
    {
        cmdLine += L" " + args;
    }

    STARTUPINFOW si{};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;

    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(exe.c_str(), const_cast<LPWSTR>(cmdLine.c_str()),
                                     nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                                     nullptr, nullptr, &si, &pi);
    if (!ok)
    {
        return false;
    }

    if (outPid != nullptr)
    {
        *outPid = pi.dwProcessId;
    }

    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const std::wstring rawCommandLine = ::GetCommandLineW();

    // 解析出参数（argc/argv）。argv[0] 是它自己（System32\explorer.exe）。
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(rawCommandLine.c_str(), &argc);
    if (argv == nullptr)
    {
        // 连参数都解析不了，直接交给系统 explorer，别挡路
        Spawn(RealExplorerPath(), SkipFirstToken(rawCommandLine.c_str()), nullptr);
        return 0;
    }

    // ---- 第一步：挑出“要打开的路径” ----
    std::wstring targetPath;   // 交给本文件管理器的路径
    bool          hasPath   = false;

    for (int i = 1; i < argc && !hasPath; ++i)
    {
        std::wstring arg = argv[i];

        // 形式一：/select,C:\path\file.txt  （最常见，微信和批量打印软件都是这个）
        if (_wcsnicmp(arg.c_str(), L"/select", 7) == 0)
        {
            if (arg.size() > 8 && arg[7] == L',')
            {
                targetPath = arg.substr(8);          // 逗号后面就是路径

                // 路径里有空格、调用方又没加引号时，系统会把它拆成好几个参数，
                // 这里按空格拼回去，尽量还原成一个完整路径
                for (int k = i + 1; k < argc; ++k)
                {
                    targetPath += L" ";
                    targetPath += argv[k];
                }
                hasPath = true;
            }
            else if (arg.size() == 8 && arg[7] == L',')
            {
                // Chromium / Electron 系（DeepSeek Harness 等）的写法：
                // 第一个参数就是 "/select,"（逗号后为空），路径在下一个参数里
                if (i + 1 < argc)
                {
                    targetPath = argv[i + 1];
                    hasPath = true;
                }
            }
            else if (arg.size() == 7 && i + 1 < argc) // 罕见的写法：/select 与路径分两个参数
            {
                targetPath = argv[i + 1];
                hasPath = true;
            }
            break;
        }

        // 形式一·补：直接传 file:///C:/... URL（没带 /select，少见但合法）
        if (_wcsnicmp(arg.c_str(), L"file:", 5) == 0)
        {
            targetPath = arg;
            hasPath = true;
            break;
        }

        // 形式二：纯路径（explorer.exe "C:\folder"）
        if (IsLocalAbsolutePath(arg))
        {
            targetPath = arg;
            hasPath = true;
            break;
        }

        // 其它以 / 开头的都是资源管理器自己的开关（/e /n /root /separate /idlist …），
        // 我们看不懂也不想接管，直接跳出，走“直通系统”那一支
        if (arg.size() > 0 && arg[0] == L'/')
        {
            break;
        }
    }

    ::LocalFree(argv);

    // 清掉路径首尾残留的引号（\"/" 之类），避免“明明文件存在却说不存在”
    if (hasPath)
    {
        targetPath = TrimPathQuotes(targetPath);
    }

    // file:///C:/... 这种 URL 转回本地路径（Electron / Chromium 系程序传的就是这个）
    if (hasPath && _wcsnicmp(targetPath.c_str(), L"file:", 5) == 0)
    {
        targetPath = FileUrlToPath(targetPath);
    }

    // ---- 第二步：决定交给谁 ----
    std::wstring ownExplorer = ResolveOwnExplorer();
    std::wstring argsToExplorer;   // 直通系统 explorer 时用的原始参数
    std::wstring decidedTo;        // 只用于日志

    if (hasPath && !ownExplorer.empty())
    {
        // 路径必须真实存在，否则当成没看懂，直通系统（保持和原来一样的表现）
        const DWORD attr = ::GetFileAttributesW(targetPath.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES)
        {
            // 交给我们：目录 → 打开该目录；文件 → 打开所在目录并选中它
            // （主程序已经支持“传文件进来就打开父目录并选中”，这里不用区分）
            const std::wstring quoted = L"\"" + targetPath + L"\"";
            DWORD pid = 0;
            if (Spawn(ownExplorer, quoted, &pid))
            {
                AppendLog(L"[YEXPLORER] 收到=\"" + rawCommandLine +
                          L"\" 路径=\"" + targetPath + L"\" pid=" + std::to_wstring(pid));
                return 0;
            }
            decidedTo = L"自己的程序启动失败，改直通系统";
        }
        else
        {
            decidedTo = L"路径不存在，直通系统";
        }
    }
    else if (!hasPath)
    {
        decidedTo = L"没有可识别的路径参数，直通系统";
    }
    else
    {
        decidedTo = L"没有登记默认文件管理器（或已恢复系统默认），直通系统";
    }

    // ---- 第三步：直通系统 explorer ----
    argsToExplorer = SkipFirstToken(rawCommandLine.c_str());
    const bool ok = Spawn(RealExplorerPath(), argsToExplorer, nullptr);

    AppendLog(L"[SYSTEM] 收到=\"" + rawCommandLine + L"\" 原因=" + decidedTo +
              (ok ? L"" : L"（连系统 explorer 都没启动成功！）"));

    // 兜底：如果我们既没交给自己、系统 explorer 也起不来，再试一次最朴素的方式
    if (!ok)
    {
        const std::wstring realExplorer = RealExplorerPath();
        ::ShellExecuteW(nullptr, L"open", realExplorer.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    return 0;
}
