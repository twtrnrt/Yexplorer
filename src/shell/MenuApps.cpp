// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/MenuApps.cpp
//
// owner → 应用 + 来源类型归组。说明见 MenuApps.h。
// ============================================================================

#include "shell/MenuApps.h"

#include "core/Logger.h"

#include <algorithm>
#include <cwctype>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>

#include <shlwapi.h>

#pragma comment(lib, "version.lib")

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

std::wstring FileVersionString(const std::wstring& path, const wchar_t* field)
{
    if (path.empty())
    {
        return std::wstring();
    }

    DWORD dummy = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (size == 0)
    {
        return std::wstring();
    }

    std::vector<BYTE> buffer(size);
    if (!::GetFileVersionInfoW(path.c_str(), 0, size, buffer.data()))
    {
        return std::wstring();
    }

    struct Translation
    {
        WORD language;
        WORD codePage;
    };

    Translation* translations = nullptr;
    UINT translationSize = 0;
    if (!::VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation",
                          reinterpret_cast<void**>(&translations), &translationSize) ||
        translations == nullptr || translationSize < sizeof(Translation))
    {
        return std::wstring();
    }

    wchar_t key[256] = {};
    ::swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\%s",
                 translations[0].language, translations[0].codePage, field);

    wchar_t* value = nullptr;
    UINT valueSize = 0;
    if (!::VerQueryValueW(buffer.data(), key, reinterpret_cast<void**>(&value), &valueSize) ||
        value == nullptr || valueSize == 0)
    {
        return std::wstring();
    }

    return std::wstring(value);
}

std::wstring ReadUtf8File(const std::wstring& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::wstring();

    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.empty()) return std::wstring();

    const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return std::wstring();

    std::wstring result(static_cast<size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                          result.data(), length);
    return result;
}

std::wstring XmlTagText(const std::wstring& xml, const wchar_t* tag)
{
    const std::wstring open = std::wstring(L"<") + tag;
    size_t start = xml.find(open);
    if (start == std::wstring::npos) return std::wstring();

    start = xml.find(L'>', start);
    if (start == std::wstring::npos) return std::wstring();
    ++start;

    const std::wstring close = std::wstring(L"</") + tag + L">";
    const size_t end = xml.find(close, start);
    if (end == std::wstring::npos) return std::wstring();

    return xml.substr(start, end - start);
}

// 从 WindowsApps\...\pkg\sub\file.dll 往上找 AppxManifest.xml
std::wstring FindPackageManifest(const std::wstring& server)
{
    std::wstring dir = server;
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    dir.resize(slash);

    for (int depth = 0; depth < 6 && !dir.empty(); ++depth)
    {
        const std::wstring manifest = dir + L"\\AppxManifest.xml";
        if (::PathFileExistsW(manifest.c_str()))
        {
            return manifest;
        }

        const size_t parent = dir.find_last_of(L"\\/");
        if (parent == std::wstring::npos) break;
        dir.resize(parent);
    }
    return std::wstring();
}

std::wstring PackagePublisher(const std::wstring& server)
{
    const std::wstring manifestPath = FindPackageManifest(server);
    if (manifestPath.empty()) return std::wstring();

    const std::wstring xml = ReadUtf8File(manifestPath);
    if (xml.empty()) return std::wstring();

    std::wstring publisher = XmlTagText(xml, L"PublisherDisplayName");
    if (publisher.empty())
    {
        // <Identity Publisher="CN=..."> 里的公司部分
        const std::wstring identity = XmlTagText(xml, L"Identity");
        (void)identity;
        const size_t pos = xml.find(L"Publisher=\"CN=&quot;");
        if (pos != std::wstring::npos)
        {
            const size_t begin = pos + wcslen(L"Publisher=\"CN=&quot;");
            const size_t end = xml.find(L"&quot;", begin);
            if (end != std::wstring::npos) publisher = xml.substr(begin, end - begin);
            const size_t comma = publisher.find(L',');
            if (comma != std::wstring::npos) publisher.resize(comma);
        }
    }
    return publisher;
}

std::wstring PackageDisplayName(const std::wstring& server)
{
    const std::wstring manifestPath = FindPackageManifest(server);
    if (manifestPath.empty()) return std::wstring();

    const std::wstring xml = ReadUtf8File(manifestPath);
    if (xml.empty()) return std::wstring();

    std::wstring name = XmlTagText(xml, L"DisplayName");
    if (name.empty() || name == L"wpsappext1")
    {
        name.clear();
    }
    return name;
}

// 归一化公司名：只留字母数字，小写；用于跨经典/现代匹配。
std::wstring NormalizeCompany(const std::wstring& company)
{
    std::wstring result;
    for (wchar_t c : company)
    {
        if (::iswalnum(c))
        {
            result.push_back(static_cast<wchar_t>(::towlower(c)));
        }
    }
    return result;
}

bool CompaniesMatch(const std::wstring& a, const std::wstring& b)
{
    if (a.size() < 4 || b.size() < 4) return false;
    return a.find(b) != std::wstring::npos || b.find(a) != std::wstring::npos;
}

std::wstring FallbackNameFromServer(const std::wstring& server)
{
    const wchar_t* name = ::PathFindFileNameW(server.c_str());
    std::wstring stem = (name != nullptr) ? name : server;
    const size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) stem.resize(dot);
    return stem.empty() ? L"未识别应用" : stem;
}

std::wstring WindowsDirectory()
{
    wchar_t dir[MAX_PATH] = {};
    const UINT length = ::GetWindowsDirectoryW(dir, MAX_PATH);
    return length > 0 ? std::wstring(dir, length) : std::wstring();
}

bool IsSystemPath(const std::wstring& path)
{
    const std::wstring windows = WindowsDirectory();
    if (windows.empty() || path.empty()) return false;

    const std::wstring lowerPath = ToLower(path);
    const std::wstring lowerWindows = ToLower(windows);
    if (lowerPath.rfind(lowerWindows + L"\\", 0) != 0)
    {
        return false;
    }

    // WindowsApps 虽然是系统目录，但里面是 Windows Terminal / 照片这类
    // 用户可能想要的现代菜单，不属于“系统自带 handler”，保留。
    if (lowerPath.find(L"\\windowsapps\\") != std::wstring::npos)
    {
        return false;
    }
    return true;
}

// 老 MenuScene 位（file=1, folder=2, multiple=4, background=8）
// → 新 MenuSceneBits（background=1, file=2, folder=4, multiple=8）
unsigned MapOwnerScenes(unsigned int scenes)
{
    unsigned result = 0;
    if (scenes & 1) result |= 1u << 1;   // file
    if (scenes & 2) result |= 1u << 2;   // folder
    if (scenes & 4) result |= 1u << 3;   // multiple
    if (scenes & 8) result |= 1u << 0;   // background
    return result;
}

struct ClassicCandidate
{
    std::wstring company;
    std::wstring appName;
};

std::wstring AppNameForOwner(const DynamicOwner& owner,
                             const std::vector<ClassicCandidate>& classics)
{
    const std::wstring product = FileVersionString(owner.server, L"ProductName");
    const std::wstring company = FileVersionString(owner.server, L"CompanyName");
    const std::wstring description = FileVersionString(owner.server, L"FileDescription");

    if (owner.kind == MenuInvokeKind::PackagedCommand)
    {
        const std::wstring publisher = PackagePublisher(owner.server);
        const std::wstring publisherKey = NormalizeCompany(publisher);
        for (const ClassicCandidate& candidate : classics)
        {
            if (CompaniesMatch(candidate.company, publisherKey))
            {
                return candidate.appName;
            }
        }

        const std::wstring display = PackageDisplayName(owner.server);
        if (!display.empty()) return display;
        if (!owner.appName.empty()) return owner.appName;
        if (!description.empty()) return description;
        return FallbackNameFromServer(owner.server);
    }

    if (!product.empty()) return product;
    if (!owner.appName.empty()) return owner.appName;
    if (!description.empty()) return description;
    return FallbackNameFromServer(owner.server);
}

} // namespace

std::wstring AppSourceKey(const std::wstring& appName, AppSourceKind source)
{
    return appName + L"|" + (source == AppSourceKind::Modern ? L"1" : L"0");
}

std::vector<AppSourceRow> BuildAppSourceRows(const std::vector<DynamicOwner>& owners)
{
    // 先收经典应用的公司/产品名，供现代包按发布者归并（WPS 经典+现代合成一条应用）
    std::vector<ClassicCandidate> classics;
    for (const DynamicOwner& owner : owners)
    {
        if (owner.kind != MenuInvokeKind::ShellHandler) continue;
        if (owner.probeStatus != L"ok") continue;
        if (IsSystemPath(owner.server)) continue;

        const std::wstring company = FileVersionString(owner.server, L"CompanyName");
        if (company.empty()) continue;

        std::wstring appName = FileVersionString(owner.server, L"ProductName");
        if (appName.empty()) appName = owner.appName;
        if (appName.empty()) continue;

        classics.push_back({ NormalizeCompany(company), appName });
    }

    std::map<std::wstring, AppSourceRow> rows;

    for (const DynamicOwner& owner : owners)
    {
        if (owner.kind != MenuInvokeKind::ShellHandler &&
            owner.kind != MenuInvokeKind::PackagedCommand)
        {
            continue;
        }
        if (owner.probeStatus != L"ok") continue;
        if (owner.server.empty() || IsSystemPath(owner.server)) continue;

        const AppSourceKind source = owner.kind == MenuInvokeKind::PackagedCommand
            ? AppSourceKind::Modern
            : AppSourceKind::Classic;

        std::wstring appName = AppNameForOwner(owner, classics);
        if (appName.empty()) appName = FallbackNameFromServer(owner.server);
        if (appName.rfind(L"ms-resource:", 0) == 0) continue;

        const std::wstring key = AppSourceKey(appName, source);
        AppSourceRow& row = rows[key];
        row.key = key;
        row.appName = appName;
        row.source = source;
        row.scenes |= MapOwnerScenes(owner.scenes);
        row.owners.push_back(owner);
    }

    std::vector<AppSourceRow> result;
    result.reserve(rows.size());
    for (auto& entry : rows)
    {
        if (entry.second.owners.empty()) continue;
        std::sort(entry.second.owners.begin(), entry.second.owners.end(),
                  [](const DynamicOwner& a, const DynamicOwner& b)
                  { return a.displayName < b.displayName; });
        result.push_back(std::move(entry.second));
    }

    std::sort(result.begin(), result.end(),
              [](const AppSourceRow& a, const AppSourceRow& b)
              {
                  if (a.appName != b.appName) return a.appName < b.appName;
                  return static_cast<int>(a.source) < static_cast<int>(b.source);
              });
    return result;
}

namespace
{

struct AppRowsCache
{
    std::vector<AppSourceRow> rows;
    bool loaded = false;
};

AppRowsCache& Cache()
{
    static AppRowsCache cache;
    return cache;
}

} // namespace

const std::vector<AppSourceRow>& CachedAppSourceRows()
{
    AppRowsCache& cache = Cache();
    if (cache.loaded)
    {
        return cache.rows;
    }

    std::vector<DynamicOwner> owners;
    if (!LoadDynamicOwnerCache(owners) || owners.empty())
    {
        Logger::Info(L"右键菜单：owner 缓存为空，开始重建（只做一次，可能稍慢）");
        BuildDynamicOwnerCache();
        LoadDynamicOwnerCache(owners);
    }

    cache.rows = BuildAppSourceRows(owners);
    cache.loaded = true;
    Logger::Info(L"右键菜单：应用行 " + std::to_wstring(cache.rows.size()) +
                 L" 条，owner " + std::to_wstring(owners.size()) + L" 个");
    return cache.rows;
}

void InvalidateAppSourceRows()
{
    Cache().loaded = false;
}

const AppSourceRow* FindAppSourceRow(const std::wstring& key)
{
    for (const AppSourceRow& row : CachedAppSourceRows())
    {
        if (row.key == key) return &row;
    }
    return nullptr;
}

} // namespace shell
