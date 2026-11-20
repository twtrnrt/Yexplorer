// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ContainerExtract.cpp
//
// 压缩包内部项的解压 / 缓存 / 临时目录管理。说明见 ContainerExtract.h。
// ============================================================================

#include "ContainerExtract.h"

#include "ShellDragDrop.h"
#include "ShellFolder.h"
#include "ShellOperations.h"
#include "core/Logger.h"
#include "core/PathUtil.h"   // MakeLongPath：包内路径很长时临时路径会超 260

#include <objbase.h>

#include <atomic>
#include <memory>
#include <thread>

namespace shell
{

namespace
{

// 本程序在 %TEMP% 下的统一根目录：解压缓存、代理临时 exe、兜底日志都收在这里，
// 不再散落在 TEMP 根目录下
const wchar_t* kTempRootName = L"Yexp";

// 解压缓存保留天数：超过这个天数的缓存目录，启动时由后台线程清掉（写死常量，不做设置项）
const int kCacheRetentionDays = 7;

// 半成品名字里的标记（两边必须是同一个，生成时也只在这里取）
//   zip 通道：正式名.yexp-partial.<进程号>.<序号>
//   rar/7z 通道：_yexp_landing_<进程号>.<序号>
const wchar_t* kPartialToken     = L".yexp-partial.";
const wchar_t* kLandingDirPrefix = L"_yexp_landing_";

// 启动清理的扫描预算：缓存很大（几万个文件）时不许它把启动后台拖长。
// 用"条目数 + 毫秒"双上限，任意一个到顶就收工，剩下的下次启动再接着清。
const size_t   kMaxCleanupScanEntries      = 30000;
const ULONGLONG kMaxCleanupScanMilliseconds = 500;

// 取临时根目录全路径（末尾带反斜杠）；取不到 TEMP 环境返回空串
std::wstring TempRoot()
{
    wchar_t tempPath[MAX_PATH] = {};
    if (::GetTempPathW(ARRAYSIZE(tempPath), tempPath) == 0)
    {
        return L"";
    }
    return std::wstring(tempPath) + kTempRootName + L"\\";
}

// 超长路径转成 \\?\ 写法（临时根 + 包内深层路径很容易顶到 260）。
// 短路径原样返回，所以调用处直接套用即可。
std::wstring LongPath(const std::wstring& path)
{
    return pathutil::MakeLongPath(path);
}

// zip 通道半成品：名字里带 ".yexp-partial." 就算（标记在名字中间，不在开头）
bool IsPartialFileName(const std::wstring& name)
{
    return name.find(kPartialToken) != std::wstring::npos;
}

// rar/7z 通道临时落地目录：名字以 "_yexp_landing_" 开头
bool IsLandingDirName(const std::wstring& name)
{
    const std::wstring prefix = kLandingDirPrefix;
    return name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0;
}

// 从名字里读出"属主进程号"，读不出给 0
//
// 为什么要读：另一个实例可能正在解压（半成品/落地目录里正写着东西），
// 这时候不能删它的中间物，否则那次解压会失败。属主进程还活着就跳过。
size_t OwnerProcessIdFromName(const std::wstring& name)
{
    std::wstring digits;

    // 半成品：... .yexp-partial.<pid>.<seq>
    if (IsPartialFileName(name))
    {
        const size_t tokenStart = name.find(kPartialToken);

        for (size_t i = tokenStart + std::wstring(kPartialToken).size(); i < name.size(); ++i)
        {
            if (name[i] >= L'0' && name[i] <= L'9')
            {
                digits += name[i];
            }
            else
            {
                break;
            }
        }
    }
    // 落地目录：_yexp_landing_<pid>.<seq>
    else if (IsLandingDirName(name))
    {
        const size_t prefixLength = std::wstring(kLandingDirPrefix).size();

        for (size_t i = prefixLength; i < name.size(); ++i)
        {
            if (name[i] >= L'0' && name[i] <= L'9')
            {
                digits += name[i];
            }
            else
            {
                break;
            }
        }
    }

    if (digits.empty() || digits.size() > 10)
    {
        return 0;
    }

    return static_cast<size_t>(std::wcstoul(digits.c_str(), nullptr, 10));
}

// 属主进程还活着吗？
//
// 规则：拿不准就当成"活着"（跳过多删），宁可少删一次也不要误删别人正在写的东西。
//   · PID 读不出来（0）→ 活着；
//   · OpenProcess 成功 → 活着；
//   · 失败原因是 ERROR_INVALID_PARAMETER（这个 PID 根本不存在）→ 死了，可以删；
//   · 其它失败（无权限等）→ 按活着处理。
bool IsOwnerProcessAlive(size_t processId)
{
    if (processId == 0)
    {
        return true;
    }

    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                   static_cast<DWORD>(processId));

    if (process != nullptr)
    {
        ::CloseHandle(process);
        return true;
    }

    return ::GetLastError() != ERROR_INVALID_PARAMETER;
}

// 清理扫描的预算（条目数 / 毫秒），跨整棵遍历共享
struct CleanupBudget
{
    size_t    scannedEntries = 0;   // 已经看过多少条
    bool      exhausted      = false;   // 是否撞到上限
    ULONGLONG startTick      = 0;   // 开始时间（GetTickCount64）
};

// 递归遍历临时目录，清掉超期缓存和半成品残留
//   root   ：当前遍历到的目录全路径
//   depth  ：相对临时根的层级（0 = 直接挂在根下的缓存目录）
//   cutoff ：超期时间点（当前时间 − 保留天数），早于它的缓存目录删掉
//   budget ：扫描预算，超了就整体收手（剩下的留到下次启动）
void CleanupTempTree(const std::wstring& root, int depth, const FILETIME& cutoff,
                     CleanupBudget& budget)
{
    if (budget.exhausted)
    {
        return;
    }

    WIN32_FIND_DATAW findData = {};
    HANDLE find = ::FindFirstFileW(LongPath(root + L"\\*").c_str(), &findData);
    if (find == INVALID_HANDLE_VALUE)
    {
        return;
    }

    do
    {
        if (++budget.scannedEntries > kMaxCleanupScanEntries ||
            ::GetTickCount64() - budget.startTick > kMaxCleanupScanMilliseconds)
        {
            // 这一棵树没看完也没关系：没删的下次再删
            budget.exhausted = true;
            break;
        }

        const std::wstring name = findData.cFileName;
        if (name == L"." || name == L"..")
        {
            continue;
        }

        const std::wstring fullPath = root + L"\\" + name;
        const bool         isDir    = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

        if (isDir)
        {
            // proxy 子目录放代理临时 exe，即用即删，不参与缓存清理
            if (depth == 0 && name == L"proxy")
            {
                continue;
            }

            // rar/7z 通道的落地目录：半成品，属主进程已经没了就整棵删掉
            if (IsLandingDirName(name))
            {
                if (!IsOwnerProcessAlive(OwnerProcessIdFromName(name)))
                {
                    shellops::RemoveTreeBestEffort(fullPath);
                }
                continue;
            }

            if (depth == 0)
            {
                // 根下的目录 = 解压缓存目录：按最后修改时间判是否超期
                if (::CompareFileTime(&findData.ftLastWriteTime, &cutoff) < 0)
                {
                    shellops::RemoveTreeBestEffort(fullPath);
                    continue;
                }
            }

            // 未超期的缓存目录 / 包内子目录：往下钻，清里面嵌套的半成品
            CleanupTempTree(fullPath, depth + 1, cutoff, budget);
        }
        else
        {
            // 兜底日志保留；其余文件只认半成品 partial，来历不明的文件不动
            if (name == L"launcher.log")
            {
                continue;
            }

            if (IsPartialFileName(name))
            {
                // 属主进程还在（另一个实例正在写这个半成品）→ 别碰
                if (IsOwnerProcessAlive(OwnerProcessIdFromName(name)))
                {
                    continue;
                }

                const std::wstring longPath = LongPath(fullPath);
                ::SetFileAttributesW(longPath.c_str(), FILE_ATTRIBUTE_NORMAL);
                ::DeleteFileW(longPath.c_str());
            }
        }
    }
    while (::FindNextFileW(find, &findData));

    ::FindClose(find);
}

// 「期望时间」：解出来的缓存文件应该带的修改时间
//
// 就是包内那一项的修改时间；包没给（全 0）时用 1980-01-01 00:00 UTC。
// 为什么是 1980：实测 NTFS 能接受的最小时间就是 1980-01-01（设 1601 会被系统钳回 1980），
// 拿它当"这个包没记时间"的标记值，设下去和读回来能对得上。
FILETIME ExpectedContainerTime(const FILETIME& containerTime)
{
    if (containerTime.dwLowDateTime != 0 || containerTime.dwHighDateTime != 0)
    {
        return containerTime;
    }

    SYSTEMTIME zeroPoint = {};
    zeroPoint.wYear   = 1980;
    zeroPoint.wMonth  = 1;
    zeroPoint.wDay    = 1;
    zeroPoint.wHour   = 0;
    zeroPoint.wMinute = 0;
    zeroPoint.wSecond = 0;

    FILETIME stamp = {};
    ::SystemTimeToFileTime(&zeroPoint, &stamp);   // 参数按 UTC 解释，正好对上 1980-01-01 00:00 UTC
    return stamp;
}

// 递归数出"容器内某个文件夹"里的所有文件（相对路径 + 大小 + 时间）
//
// 只给文件夹项用。整目录复制（CopyHere）中途拿不到"此刻复制到哪个文件"，
// 所以解压之前先把清单录一遍；复制过程中每秒对着清单问一次"这个文件出现了没有、
// 长了多少"，就能算出真实进度，还能看出当前正在写的是哪个文件。
//
// 只录一次、之后不再遍历目录树 —— 每秒只对清单里那些已知路径做一次 stat，
// 1000 个文件也只要 1~3ms，基本无感。
void CollectContainerFiles(const std::wstring& folderVirtualPath,
                           const std::wstring& relativeRoot,
                           std::vector<std::wstring>& outPaths,
                           std::vector<unsigned long long>& outSizes,
                           std::vector<FILETIME>& outTimes,
                           int depth)
{
    if (depth > 32)
    {
        return;   // 防御：容器里万一是环形结构，别把栈跑爆
    }

    EnumerateResult children = EnumerateFolder(folderVirtualPath, nullptr);

    if (FAILED(children.hr))
    {
        return;
    }

    for (const auto& entry : children.entries)
    {
        if (entry.isGroupHeader || entry.isSpacer)
        {
            continue;
        }

        std::wstring relative = relativeRoot;

        if (!relative.empty() && relative.back() != L'\\')
        {
            relative += L'\\';
        }

        relative += entry.name;

        if (entry.isFolder)
        {
            CollectContainerFiles(entry.fullPath, relative, outPaths, outSizes, outTimes, depth + 1);
        }
        else
        {
            outPaths.push_back(relative);
            outSizes.push_back(entry.size);

            // 包没给时间时统一走 1980 兜底，和单文件项用的是同一套规范化，
            // 这样"整目录解压"和"单独打开里面某个文件"盖出来的是同一个值
            outTimes.push_back(ExpectedContainerTime(entry.modified));
        }
    }
}

} // namespace

// ============================================================================
// 临时目录
// ============================================================================

// 某个压缩包里解出来的东西落在哪个临时目录
//
// 目录名 = 包路径hash8_包字节数_包修改时间(YYYYMMDDHHMMSS)_包名尾部24字符
// 例：%TEMP%\Yexp\A1B2C3D4_52428800_20260101120000_文档转换测试.zip
//
// 为什么这么起名：
//   1) 同一个包（路径、大小、修改时间都没变）永远落同一个目录 → 上次解出来的文件能复用；
//      包被替换/更新后目录名就变 → 旧缓存自动失效，不需要再记"这是不是同一个包"。
//   2) 进程重启后还是同一个目录 → 缓存跨启动也能用；%TEMP% 里一个包只留一个目录，
//      不会像以前那样每启动一次就多堆一个。
//   3) 前缀放"路径 hash"是为了区分不同路径下的同名包（release.zip 这种太常见）；
//      存 hash 不存路径明文，名字短，也不暴露路径。
//   4) 包名只留尾部 24 字符（保住扩展名）：%TEMP% 前缀 + 目录名 + 包内相对路径
//      要一起塞进 260 以内，名字长的包会把包内路径顶爆。
std::wstring TempDirectoryFor(const std::wstring& containerRoot)
{
    if (containerRoot.empty())
    {
        return L"";
    }

    // ---- 1) 包的身份：字节数 + 修改时间 ----
    //
    // 拿不到（包刚被删、没权限）就当 0：目录名退化成一个占位目录，
    // 之后真去解压时该报错还是会报错。
    unsigned long long containerBytes    = 0;
    FILETIME           containerModified = {};

    WIN32_FILE_ATTRIBUTE_DATA containerAttributes = {};
    if (::GetFileAttributesExW(LongPath(containerRoot).c_str(),
                               ::GetFileExInfoStandard, &containerAttributes))
    {
        containerBytes = (static_cast<unsigned long long>(containerAttributes.nFileSizeHigh) << 32) |
                         static_cast<unsigned long long>(containerAttributes.nFileSizeLow);
        containerModified = containerAttributes.ftLastWriteTime;
    }

    // ---- 2) 包路径的短 hash（FNV-1a 32 位；先统一成小写，Windows 路径不分大小写）----
    unsigned int pathHash = 2166136261u;   // FNV-1a 32 位初始值

    for (wchar_t ch : containerRoot)
    {
        if (ch >= L'A' && ch <= L'Z')
        {
            ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
        }

        pathHash = (pathHash ^ static_cast<unsigned int>(ch & 0xFFFF)) * 16777619u;
    }

    // ---- 3) 包修改时间 → 本地时间字符串 YYYYMMDDHHMMSS ----
    FILETIME localModified = {};
    ::FileTimeToLocalFileTime(&containerModified, &localModified);

    SYSTEMTIME modifiedParts = {};
    ::FileTimeToSystemTime(&localModified, &modifiedParts);

    wchar_t timeText[32] = {};
    ::swprintf_s(timeText, L"%04u%02u%02u%02u%02u%02u",
                 modifiedParts.wYear, modifiedParts.wMonth, modifiedParts.wDay,
                 modifiedParts.wHour, modifiedParts.wMinute, modifiedParts.wSecond);

    // ---- 4) 包名只留尾部 24 字符（扩展名在最后，所以从扩展名往左数 24 个）----
    std::wstring containerName = containerRoot;
    const size_t nameSeparator = containerName.find_last_of(L"\\/");
    if (nameSeparator != std::wstring::npos)
    {
        containerName = containerName.substr(nameSeparator + 1);
    }

    if (containerName.size() > 24)
    {
        containerName = containerName.substr(containerName.size() - 24);
    }

    // ---- 5) 拼成目录名 ----
    // 临时根只取一次（TempRoot 已经带末尾反斜杠），不再自己调 GetTempPathW，
    // 免得两处口径不一致（改根目录名时漏改这里）
    const std::wstring root = TempRoot();

    if (root.empty())
    {
        return L"";
    }

    wchar_t hashText[16] = {};
    ::swprintf_s(hashText, L"%08X", pathHash);

    return root + hashText + L"_" +
           std::to_wstring(containerBytes) + L"_" + timeText + L"_" + containerName;
}

// 解压真的往某个临时目录里写过东西之后，把这个目录自己的修改时间刷成"现在"
//
// 为什么要做：7 天保留判的是缓存目录的修改时间，而 Windows 只在"直接子项增删"时
// 才更新目录时间。如果包里是一个顶层文件夹，之后往里解别的文件只更新子文件夹，
// 外层缓存目录的时间会停在第一次解压那天 —— 明明这几天一直在写，7 天后却被当成
// "很久没写过"整个删掉。
// 只在"真的解压（写入）"之后调用；命中缓存是纯读取，不刷新（保留口径仍然是写入时间）。
void RefreshTempDirectoryTime(const std::wstring& directory)
{
    if (directory.empty())
    {
        return;
    }

    HANDLE dir = ::CreateFileW(LongPath(directory).c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);

    if (dir == INVALID_HANDLE_VALUE)
    {
        return;   // 目录刚被清掉之类：不影响本次使用
    }

    SYSTEMTIME nowSystem = {};
    ::GetSystemTime(&nowSystem);

    FILETIME now = {};
    ::SystemTimeToFileTime(&nowSystem, &now);

    ::SetFileTime(dir, nullptr, nullptr, &now);
    ::CloseHandle(dir);
}

// 这个路径是不是落在本程序的临时根目录（%TEMP%\Yexp\）下面。
// 主窗口用它判断"枚举失败"是不是因为临时解压缓存被清理了。
bool IsUnderTempRoot(const std::wstring& path)
{
    const std::wstring root = TempRoot();
    if (root.empty() || path.empty())
    {
        return false;
    }

    // 统一成"末尾带反斜杠"再比：C:\...\Yexp 和 C:\...\Yexp\ 都算在根下
    std::wstring normalized = path;
    if (normalized.back() != L'\\' && normalized.back() != L'/')
    {
        normalized += L'\\';
    }

    // 长路径前缀（\\?\）去掉再比（枚举出来的路径一般没有，这里兜个底）
    if (normalized.size() >= 4 && normalized.compare(0, 4, L"\\\\?\\") == 0)
    {
        normalized.erase(0, 4);
    }

    return ::_wcsnicmp(normalized.c_str(), root.c_str(), root.size()) == 0;
}

// 后台清理入口：扫描临时根，删超期缓存目录和半成品残留；删不掉（占用/无权限）就跳过
void CleanupTempCache()
{
    const std::wstring root = TempRoot();
    if (root.empty())
    {
        return;
    }

    // 根目录还没建过（从没解过压、也没走过兜底）就直接返回
    if (::GetFileAttributesW(LongPath(root).c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        return;
    }

    // 保留天数换算成 FILETIME 的 100 纳秒单位，算出超期时间点
    const ULONGLONG retention = static_cast<ULONGLONG>(kCacheRetentionDays) *
                                24ull * 60ull * 60ull * 10000000ull;

    FILETIME nowFt{};
    ::GetSystemTimeAsFileTime(&nowFt);

    ULARGE_INTEGER now{};
    now.LowPart  = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;
    now.QuadPart -= retention;

    FILETIME cutoff{};
    cutoff.dwLowDateTime  = now.LowPart;
    cutoff.dwHighDateTime = now.HighPart;

    CleanupBudget budget;
    budget.startTick = ::GetTickCount64();

    Logger::Info(L"临时缓存清理：扫描 " + root);
    CleanupTempTree(root, 0, cutoff, budget);

    if (budget.exhausted)
    {
        // 缓存很大：本次只清到预算为止，剩下的下次启动继续（不会越积越多）
        Logger::Warn(L"临时缓存清理：本次达到扫描上限（已看 " +
                     std::to_wstring(budget.scannedEntries) + L" 项），剩余留到下次启动");
    }
    else
    {
        Logger::Info(L"临时缓存清理：完成（共看 " +
                     std::to_wstring(budget.scannedEntries) + L" 项）");
    }
}

// ============================================================================
// 缓存
// ============================================================================

bool CacheIsUsable(const std::wstring& outputPath,
                   unsigned long long declaredSize,
                   const FILETIME&   expectedTime,
                   const std::vector<std::wstring>& childRelativePaths,
                   const std::vector<unsigned long long>& childSizes)
{
    if (outputPath.empty())
    {
        return false;
    }

    WIN32_FILE_ATTRIBUTE_DATA fileData = {};
    if (!::GetFileAttributesExW(LongPath(outputPath).c_str(), ::GetFileExInfoStandard, &fileData))
    {
        return false;   // 不在 → 没有缓存
    }

    if ((fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        // ---- 文件夹项：目录在、目录时间对、清单里每个文件都在且大小对 ----
        //
        // 整目录解压收尾时给这个目录盖过"包内目录时间"，所以时间对得上就等于
        // "这是我们上次解出来的那一份"。里面每个文件再逐个核对大小，
        // 少一个、少半截（解压被打断）都会判成不可用。
        if (::CompareFileTime(&fileData.ftLastWriteTime, &expectedTime) != 0)
        {
            return false;
        }

        for (size_t child = 0; child < childRelativePaths.size(); ++child)
        {
            const std::wstring candidate = outputPath + L"\\" + childRelativePaths[child];

            WIN32_FILE_ATTRIBUTE_DATA childData = {};
            if (!::GetFileAttributesExW(LongPath(candidate).c_str(), ::GetFileExInfoStandard, &childData))
            {
                return false;   // 少了其中一个文件
            }

            if ((childData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                return false;   // 该是文件的位置成了目录
            }

            // 清单里那份大小（包内声明）和实际对不上 → 没写完整
            if (child < childSizes.size() && childSizes[child] > 0)
            {
                const unsigned long long actualSize =
                    (static_cast<unsigned long long>(childData.nFileSizeHigh) << 32) |
                    static_cast<unsigned long long>(childData.nFileSizeLow);

                if (actualSize != childSizes[child])
                {
                    return false;
                }
            }
        }

        return true;
    }

    const unsigned long long actualSize =
        (static_cast<unsigned long long>(fileData.nFileSizeHigh) << 32) |
        static_cast<unsigned long long>(fileData.nFileSizeLow);

    // 包声明了大小 → 必须一字节不差：内容是顺序写到写满为止，写一半必然小于声明值
    if (declaredSize > 0 && actualSize != declaredSize)
    {
        return false;
    }

    // 实际 0 字节：包明确声明 0（= 空文件）才算有效缓存，靠下面的时间戳再验一次。
    // 包没给大小（声明 0 也可能是"没读到"）时不敢认——解压中断最常见的就是留下 0 字节文件。
    if (actualSize == 0 && declaredSize == 0)
    {
        // 时间对得上 = 是我们发布后盖过时间戳的那一份；对不上按不可用处理
        return (::CompareFileTime(&fileData.ftLastWriteTime, &expectedTime) == 0);
    }

    // 时间必须等于"包内那一项的修改时间"：
    // 中途丢弃的文件，时间是"写入那一刻"，永远对不上这个值。
    return (::CompareFileTime(&fileData.ftLastWriteTime, &expectedTime) == 0);
}

void StampFileTime(const std::wstring& path, const FILETIME& time)
{
    if (path.empty())
    {
        return;
    }

    // 解出来的文件默认是只读的（保护原件不被改坏），而 SetFileTime 对只读文件会失败，
    // 所以先临时放开只读，盖完时间戳再恢复。
    const std::wstring longPath   = LongPath(path);
    const DWORD        attributes = ::GetFileAttributesW(longPath.c_str());
    const bool         wasReadOnly = (attributes != INVALID_FILE_ATTRIBUTES) &&
                                     ((attributes & FILE_ATTRIBUTE_READONLY) != 0);

    if (wasReadOnly)
    {
        ::SetFileAttributesW(longPath.c_str(), attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
    }

    // 目录和文件的打开方式不一样：打开目录必须带 FILE_FLAG_BACKUP_SEMANTICS，
    // 否则 CreateFileW 一律失败（文件夹项的缓存判据就靠这里盖上的时间，盖不上等于永远不命中）
    DWORD openFlags = FILE_ATTRIBUTE_NORMAL;

    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        openFlags |= FILE_FLAG_BACKUP_SEMANTICS;
    }

    HANDLE file = ::CreateFileW(longPath.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr,
                                OPEN_EXISTING, openFlags, nullptr);

    bool stamped = false;
    if (file != INVALID_HANDLE_VALUE)
    {
        FILETIME stamp = time;
        stamped = (::SetFileTime(file, nullptr, nullptr, &stamp) != 0);
        ::CloseHandle(file);
    }

    if (wasReadOnly)
    {
        ::SetFileAttributesW(longPath.c_str(), attributes);   // 恢复原来的只读
    }

    if (!stamped)
    {
        // 盖不上不影响本次使用：下次这条会被判成"对不上"，重解一次而已
        Logger::Warn(L"解压：给缓存文件盖修改时间失败，下次会重新解压：" + path);
    }
}

// ============================================================================
// 构造"要解的项"
// ============================================================================

bool BuildItems(const std::vector<std::wstring>& paths,
                const ContainerMetaProvider&      meta,
                std::vector<ContainerExtractItem>& items,
                unsigned long long&                totalBytes)
{
    items.clear();
    totalBytes = 0;

    if (!meta)
    {
        Logger::Error(L"解压：没有提供条目元数据查询，无法计算解压落点");
        return false;
    }

    // 同一进程里可能有两个解压同时在跑（拖拽途中又双击了一个文件），
    // 中间名里带一个自增序号，保证两边不会撞同一个临时文件/临时目录
    static std::atomic<unsigned int> extractSerial{ 0 };

    for (const auto& path : paths)
    {
        ContainerExtractItem item;
        item.virtualPath = path;

        // 往上找容器根（压缩包本身）：C:\a\t.rar\sub\inner.txt → C:\a\t.rar
        item.containerRoot = ContainerRootForPath(path);

        if (item.containerRoot.empty())
        {
            Logger::Warn(L"解压：找不到这项所属的压缩包，已跳过：" + path);
            continue;
        }

        // 包内相对路径（去掉前导分隔符，保留里面的子目录结构）
        item.innerPath = path.substr(item.containerRoot.size());

        while (!item.innerPath.empty() &&
               (item.innerPath.front() == L'\\' || item.innerPath.front() == L'/'))
        {
            item.innerPath.erase(item.innerPath.begin());
        }

        item.tempDirectory = TempDirectoryFor(item.containerRoot);

        if (item.tempDirectory.empty() || item.innerPath.empty())
        {
            Logger::Warn(L"解压：算不出这项的解压落点，已跳过：" + path);
            continue;
        }

        // ---- 正式落点 = 临时目录 + 包内相对路径 ----
        item.outputPath = item.tempDirectory;

        if (item.outputPath.back() != L'\\' && item.outputPath.back() != L'/')
        {
            item.outputPath += L'\\';
        }
        item.outputPath += item.innerPath;

        for (wchar_t& ch : item.outputPath)
        {
            if (ch == L'/')
            {
                ch = L'\\';   // 统一成反斜杠，后面查大小、改名都用它
            }
        }

        // ---- 两条通道各自的中间路径 ----
        //   zip：先写到 partialPath，写完改名成 outputPath
        //   rar：先复制到 stagingPath 目录，收尾后再挪成 outputPath
        // 进度就是盯着它们："正在长的那个文件多大" = 已经写了多少字节
        const std::wstring tag = std::to_wstring(::GetCurrentProcessId()) + L"." +
                                 std::to_wstring(extractSerial.fetch_add(1) + 1);

        item.partialPath = item.outputPath + L".yexp-partial." + tag;

        const size_t lastSeparator = item.outputPath.find_last_of(L'\\');
        const std::wstring outputName =
            (lastSeparator == std::wstring::npos) ? item.outputPath
                                                  : item.outputPath.substr(lastSeparator + 1);

        // 落地目录放在"缓存根"上，而不是 outputPath 的父目录：
        // rar/7z 那条通道走 Shell.Application 的 CopyHere，它用的是 Shell 解析名，
        // 不认 \\?\ 前缀 —— 落地目录越深越容易顶到 260 而直接打不开。
        // 放到缓存根就只有 "临时根 + 指纹目录" 那一层，留给包内路径的余量最大化。
        // 每个 tag 一个专属目录，两个并发解压不会互相踩。
        item.stagingPath = item.tempDirectory + L"\\_yexp_landing_" + tag;
        item.landingPath = item.stagingPath + L"\\" + outputName;

        // ---- 该有多大、该是什么时间（缓存判据也靠这两个）----
        ContainerEntryMeta entryMeta;

        if (!meta(path, entryMeta))
        {
            Logger::Warn(L"解压：取不到这项的大小/时间，已跳过：" + path);
            continue;
        }

        item.expectedSize = entryMeta.size;
        item.expectedTime = ExpectedContainerTime(entryMeta.modified);

        // 文件夹没有"大小"，元数据给的是 0；这里把里面的文件清单录一遍，
        // 用清单的总字节当进度分母，复制过程中对着清单算已写入多少。
        // （空文件夹清单为空、总字节 0，进度退回按项数走，和以前一样）
        if (entryMeta.isFolder)
        {
            CollectContainerFiles(path, L"", item.childRelativePaths, item.childSizes,
                                  item.childTimes, 0);

            item.expectedSize = 0;

            for (unsigned long long childSize : item.childSizes)
            {
                item.expectedSize += childSize;
            }
        }

        totalBytes += item.expectedSize;
        items.push_back(std::move(item));
    }

    return !items.empty();
}

// ============================================================================
// 解压执行器
// ============================================================================

// 主线程在这里"边等边刷屏幕"，真正的解压在后台线程里做 ——
// CopyHere 是同步阻塞的（实测 652MB 要十几秒），放在主线程里刷不出任何提示。
bool RunExtraction(const std::vector<ContainerExtractItem>& items,
                   bool                                   showMouseTip,
                   const std::function<void()>&                  onBegin,
                   const std::function<void(const std::wstring&)>& onProgress,
                   const std::function<void()>&                  onEnd,
                   std::vector<std::wstring>&             realPaths,
                   std::wstring&                          lastError)
{
    realPaths.clear();
    lastError.clear();

    if (items.empty())
    {
        return true;
    }

    // ---- 1) 先把命中缓存的挑出来（它们不用解）----
    std::vector<const ContainerExtractItem*> pending;

    for (const auto& item : items)
    {
        if (CacheIsUsable(item.outputPath, item.expectedSize, item.expectedTime,
                          item.childRelativePaths, item.childSizes))
        {
            Logger::Info(L"解压：命中缓存，直接复用：" + item.virtualPath);

            // 命中的项统一维持只读：打开时保护原件；拖拽那边在拖动前会临时改成普通
            const std::wstring longOutputPath = LongPath(item.outputPath);
            const DWORD cacheAttributes = ::GetFileAttributesW(longOutputPath.c_str());
            if (cacheAttributes != INVALID_FILE_ATTRIBUTES)
            {
                ::SetFileAttributesW(longOutputPath.c_str(),
                                     cacheAttributes | FILE_ATTRIBUTE_READONLY);
            }

            realPaths.push_back(item.outputPath);
        }
        else
        {
            pending.push_back(&item);
        }
    }

    if (pending.empty())
    {
        // 全部命中：瞬间完成，不显示任何解压文字（否则会闪一下）
        return true;
    }

    // ---- 2) 要解的总字节（只有真要解的项才占进度）----
    unsigned long long totalBytes = 0;

    for (const auto* item : pending)
    {
        totalBytes += item->expectedSize;
    }

    // ---- 3) 开始显示 ----
    if (onBegin)
    {
        onBegin();
    }

    const size_t totalItems = pending.size();

    // 只有"按着左键拖出来"才需要判断松手；
    // 不是鼠标拖的情况（触摸合成的拖动）就别乱判，否则会把正常拖动当成取消废掉
    const bool trackLeftButton = showMouseTip &&
                                 ((::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);

    // 有没有取消请求：ESC 任何时候都算；"松开左键"只在确实是拖出来的情况下算
    const auto cancelledNow = [trackLeftButton]() -> bool
    {
        return (::GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0 ||
               (trackLeftButton && ((::GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0));
    };

    unsigned long long finishedBytes = 0;   // 已经解完的项累计字节
    bool               aborted       = false;

    for (size_t index = 0; index < totalItems; ++index)
    {
        const ContainerExtractItem& item = *pending[index];

        // ---- 每项一个后台线程（COM 要先初始化成 STA）----
        struct ExtractJob
        {
            std::atomic<bool> finished{ false };
            std::wstring      realPath;
            std::wstring      error;
        };

        auto job        = std::make_shared<ExtractJob>();
        auto cancelFlag = std::make_shared<std::atomic<bool>>(false);

        std::thread worker([item, job, cancelFlag]()
        {
            const HRESULT comResult = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

            job->realPath = shellops::ExtractContainerEntry(item.virtualPath, item.containerRoot,
                                                            item.innerPath, item.tempDirectory,
                                                            item.partialPath, item.stagingPath,
                                                            cancelFlag.get(), job->error);

            // 一解出来就盖上"包内修改时间"，不管调用方还要不要这一项。
            // 意义在于：取消时停不下来、后来才补完的那一项，也会变成"合法缓存"，
            // 下次拖动/双击同一项直接命中，不用再解一遍。
            if (!job->realPath.empty())
            {
                StampFileTime(job->realPath, item.expectedTime);

                // 文件夹项：把里面每个文件也盖上"包内那一项的时间"。
                // 这样以后单独打开/拖出目录里的某个文件时，它的缓存判据（时间对得上）
                // 也能命中 —— 文件和文件夹共用同一份缓存，不用为了一个文件再整目录解一遍。
                // 只在第一次解压时做一次，之后命中缓存就不会走到这里。
                for (size_t child = 0; child < item.childRelativePaths.size() &&
                                       child < item.childTimes.size(); ++child)
                {
                    StampFileTime(job->realPath + L"\\" + item.childRelativePaths[child],
                                  item.childTimes[child]);
                }

                // 往这个缓存目录里真的写过东西了：把目录自己的修改时间刷成"现在"，
                // 让 7 天保留期从"最后一次写入"开始算（见 RefreshTempDirectoryTime 说明）
                RefreshTempDirectoryTime(item.tempDirectory);
            }

            if (SUCCEEDED(comResult))
            {
                ::CoUninitialize();
            }

            job->finished = true;
        });
        worker.detach();

        // ---- 主线程：每 50ms 一次，刷状态栏 ----
        //
        // 单文件项：每 50ms 读一次候选文件的大小，成本极低。
        // 文件夹项：清单可能很长（几千个文件），限成每秒扫一次，其余时间沿用上次结果。
        unsigned long long lastListScanTick = 0;
        unsigned long long listWrittenBytes = 0;

        while (!job->finished)
        {
            const unsigned long long nowTick      = ::GetTickCount64();
            unsigned long long       writtenBytes = 0;

            if (item.childRelativePaths.empty())
            {
                // ---- 单文件项：已写字节 = 三个候选里最大的那个 ----
                //   partialPath（zip 的中间文件）
                //   landingPath（rar 落地后的那个文件）
                //   outputPath （已经改名完成的正式文件）
                const std::wstring* candidates[3] = { &item.partialPath, &item.landingPath,
                                                      &item.outputPath };

                for (const std::wstring* candidate : candidates)
                {
                    WIN32_FILE_ATTRIBUTE_DATA fileData = {};

                    if (::GetFileAttributesExW(LongPath(*candidate).c_str(),
                                               ::GetFileExInfoStandard, &fileData))
                    {
                        const unsigned long long bytes =
                            (static_cast<unsigned long long>(fileData.nFileSizeHigh) << 32) |
                            static_cast<unsigned long long>(fileData.nFileSizeLow);

                        if (bytes > writtenBytes)
                        {
                            writtenBytes = bytes;
                        }
                    }
                }
            }
            else if (nowTick - lastListScanTick >= 1000)
            {
                // ---- 文件夹项：每秒对着清单问一次"每个文件出现了没有、长了多少" ----
                //
                // 只看清单里已经知道的那些路径，不去遍历目录树。
                lastListScanTick = nowTick;
                listWrittenBytes = 0;

                for (size_t child = 0; child < item.childRelativePaths.size(); ++child)
                {
                    const std::wstring& relative = item.childRelativePaths[child];

                    // 两个位置都要看：
                    //   rar 通道：先落在专属临时目录里（landingPath 就是那个目录）
                    //   发布之后：在正式位置（outputPath 就是那个目录）
                    const std::wstring landingCandidate = item.landingPath + L"\\" + relative;
                    const std::wstring finalCandidate   = item.outputPath + L"\\" + relative;

                    unsigned long long bytes = 0;

                    WIN32_FILE_ATTRIBUTE_DATA fileData = {};

                    if (::GetFileAttributesExW(LongPath(landingCandidate).c_str(),
                                               ::GetFileExInfoStandard, &fileData))
                    {
                        bytes = (static_cast<unsigned long long>(fileData.nFileSizeHigh) << 32) |
                                static_cast<unsigned long long>(fileData.nFileSizeLow);
                    }

                    if (::GetFileAttributesExW(LongPath(finalCandidate).c_str(),
                                               ::GetFileExInfoStandard, &fileData))
                    {
                        const unsigned long long finalBytes =
                            (static_cast<unsigned long long>(fileData.nFileSizeHigh) << 32) |
                            static_cast<unsigned long long>(fileData.nFileSizeLow);

                        if (finalBytes > bytes)
                        {
                            bytes = finalBytes;
                        }
                    }

                    const unsigned long long declared = item.childSizes[child];

                    listWrittenBytes += (declared > 0 && bytes > declared) ? declared : bytes;
                }
            }

            if (!item.childRelativePaths.empty())
            {
                writtenBytes = listWrittenBytes;
            }

            const unsigned long long expected  = (item.expectedSize > 0) ? item.expectedSize
                                                                         : totalBytes;
            const unsigned long long cappedDone = (expected > 0 && writtenBytes > expected)
                                                  ? expected
                                                  : writtenBytes;

            int percent = 0;

            if (totalBytes > 0)
            {
                const unsigned long long overallDone = finishedBytes + cappedDone;
                percent = static_cast<int>((overallDone * 100ull) / totalBytes);
            }
            else
            {
                // 包一个大小都没给：退回按项数估
                percent = static_cast<int>((index * 100ull) / totalItems);
            }

            if (percent > 100)
            {
                percent = 100;
            }

            // 文案统一按拖拽那一套：带项数，取消提示跟在百分比后面（不再分左右两格）
            // 提示条只有这一句，单文件项和文件夹项都不额外拼文件名
            const std::wstring text = L"正在解压… 第 " + std::to_wstring(index + 1) + L"/" +
                                      std::to_wstring(totalItems) + L" 项 " +
                                      std::to_wstring(percent) + L"%";

            if (onProgress)
            {
                onProgress(text + L" (按 ESC 取消)");
            }

            // 鼠标右下角提示条：只有拖拽才显示，多说一句"松开鼠标可取消"
            if (showMouseTip)
            {
                ShowDragStatusTip(text + L" (松开鼠标可取消)");
            }

            // 取消：ESC（双击、拖拽都支持）；拖拽额外支持"松开左键"
            if (cancelledNow())
            {
                aborted = true;
                cancelFlag->store(true);   // 能真停的通道立刻收手，停不了的会把结果当缓存
                break;
            }

            // 别一觉睡 200ms：小文件就在这段空隙里解完了，等于一次都没查。
            // 拆成 4 段 50ms，每段查一次 → 松手后最迟 50ms 就通知到线程。
            for (int slice = 0; slice < 4; ++slice)
            {
                if (job->finished)
                {
                    break;
                }

                ::Sleep(50);

                if (cancelledNow())
                {
                    aborted = true;
                    cancelFlag->store(true);
                    break;
                }
            }
        }

        // 线程可能就在上面那一小段空隙里解完了（while 一次都没进）。
        // 收结果之前再查一次：已经松手/按了 ESC，这一项也按取消处理。
        if (!aborted && cancelledNow())
        {
            aborted = true;
            cancelFlag->store(true);
        }

        if (aborted)
        {
            // 这里不去删任何中间物：能真停的通道（zip 读流）自己会把半成品删干净；
            // 停不了的通道（rar 的 CopyHere）会把已经复制完的结果发布到正式名当缓存。
            // 在这儿删反而会跟后台线程抢同一个目录，越删越乱。
            break;
        }

        if (job->realPath.empty())
        {
            // 解不出来就跳过这一项，其余照常（日志里留个原因）
            lastError = job->error;
            Logger::Warn(L"解压：压缩包内项解不出来，已跳过：" + item.virtualPath +
                         L"，原因：" + (job->error.empty() ? L"未知" : job->error));
            continue;
        }

        // 时间戳已经在后台线程里盖好了（见 worker 里的说明），这里只收结果
        realPaths.push_back(job->realPath);

        finishedBytes += item.expectedSize;
    }

    // ---- 收尾：撤状态栏、撤鼠标提示条 ----
    if (showMouseTip)
    {
        HideDragStatusTip();
    }

    if (onEnd)
    {
        onEnd();
    }

    if (aborted)
    {
        // 已解完的项留在临时目录当缓存，下次直接命中
        Logger::Info(L"解压：途中取消（已解出 " + std::to_wstring(realPaths.size()) + L" 项）");
        return false;
    }

    Logger::Info(L"解压：完成 " + std::to_wstring(realPaths.size()) + L" / " +
                 std::to_wstring(items.size()) + L" 项");
    return true;
}

} // namespace shell
