// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ContextMenuCollector.cpp
//
// 运行时采集 shellex 真实菜单项 + 缓存 JSON 读写实现。
//
// 采集原理（和资源管理器右键菜单同一套 COM）：
//   · 文件 / 文件夹：IShellItem::BindToHandler(BHID_SFUIObject) 拿 IContextMenu；
//   · 背景目录：先拿目录的 IShellFolder，GetUIObjectOf 传空 PIDL 拿背景菜单；
//   · QueryContextMenu 填临时 HMENU，递归遍历取文字 / 层级 / 动词；
//   · 子菜单先 HandleMenuMsg(WM_INITMENUPOPUP) 触发扩展懒填充。
//
// 注意：本模块在「配置右键菜单」窗口打开 / 点刷新时调用，不进右键弹出链路。
// ============================================================================

#include "shell/ContextMenuCollector.h"

#include "core/Logger.h"

#include <wil/com.h>
#include <wil/result.h>

#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <nlohmann/json.hpp>

#include <fstream>
#include <algorithm>
#include <cwctype>

#pragma comment(lib, "shlwapi.lib")

namespace shell
{

namespace
{

// ---------------------------------------------------------------------------
// 路径 / 编码 / 时间小工具
// ---------------------------------------------------------------------------

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return std::string();
    }
    int length = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                       static_cast<int>(text.size()),
                                       nullptr, 0, nullptr, nullptr);
    if (length <= 0)
    {
        return std::string();
    }
    std::string result(static_cast<size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                          static_cast<int>(text.size()),
                          result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(const std::string& text)
{
    if (text.empty())
    {
        return std::wstring();
    }
    int length = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                       static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
    {
        return std::wstring();
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                          static_cast<int>(text.size()), result.data(), length);
    return result;
}

// 缓存文件：%APPDATA%\FileExplorer\menu-cache.json
std::wstring CacheFilePath()
{
    LPWSTR appData = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) ||
        appData == nullptr)
    {
        return std::wstring();
    }
    std::wstring path = std::wstring(appData) + L"\\FileExplorer\\menu-cache.json";
    ::CoTaskMemFree(appData);
    return path;
}

bool EnsureDirectory(const std::wstring& filePath)
{
    const size_t slash = filePath.find_last_of(L'\\');
    if (slash == std::wstring::npos)
    {
        return false;
    }
    const std::wstring folder = filePath.substr(0, slash);
    int result = ::SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS;
}

// 当前本地时间 "YYYY-MM-DD HH:MM:SS"
std::wstring NowTimeString()
{
    SYSTEMTIME st = {};
    ::GetLocalTime(&st);
    wchar_t buffer[32] = {};
    ::swprintf_s(buffer, L"%04d-%02d-%02d %02d:%02d:%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

// 菜单文字里的助记符下划线标记（&F）要去掉，展示用
std::wstring StripAmpersand(std::wstring text)
{
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == L'&' && i + 1 < text.size() && text[i + 1] != L'&')
        {
            continue; // 跳过这个 &，后面字符保留
        }
        out.push_back(text[i]);
    }
    return out;
}

std::wstring ToLower(std::wstring text)
{
    for (wchar_t& ch : text)
    {
        ch = static_cast<wchar_t>(::towlower(ch));
    }
    return text;
}

// 按归并键找分组，找不到新建
ContextMenuGroup& FindOrAddGroup(ContextMenuReport& report,
                                 const std::wstring& sourceKey,
                                 const std::wstring& displayName,
                                 bool isShellex)
{
    for (ContextMenuGroup& group : report.groups)
    {
        if (group.sourceKey == sourceKey)
        {
            return group;
        }
    }
    ContextMenuGroup group;
    group.sourceKey = sourceKey;
    group.name      = displayName;
    group.isShellex = isShellex;
    report.groups.push_back(std::move(group));
    return report.groups.back();
}

// ---------------------------------------------------------------------------
// 临时样本目录：%TEMP%\FileExplorerMenuSample
//
// 一期采集三类样本：
//   · 文件：sample.txt / sample.docx / sample.rar / sample.zip（空文件）
//   · 文件夹：sample-folder
//   · 背景：样本根目录本身
// 用完整个目录清掉。
// ---------------------------------------------------------------------------
std::wstring SampleRootPath()
{
    wchar_t tempPath[MAX_PATH] = {};
    if (::GetTempPathW(ARRAYSIZE(tempPath), tempPath) == 0)
    {
        return std::wstring();
    }
    return std::wstring(tempPath) + L"FileExplorerMenuSample";
}

// 递归删除样本目录（只删我们自己建的内容）
void CleanupSample(const std::wstring& root)
{
    if (root.empty())
    {
        return;
    }

    WIN32_FIND_DATAW findData = {};
    const std::wstring pattern = root + L"\\*";
    HANDLE find = ::FindFirstFileW(pattern.c_str(), &findData);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            const std::wstring name = findData.cFileName;
            if (name == L"." || name == L"..")
            {
                continue;
            }
            const std::wstring child = root + L"\\" + name;
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                CleanupSample(child);
            }
            else
            {
                ::SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                ::DeleteFileW(child.c_str());
            }
        } while (::FindNextFileW(find, &findData));
        ::FindClose(find);
    }
    ::RemoveDirectoryW(root.c_str());
}

// 建样本目录和空文件；返回是否成功
bool PrepareSample(std::wstring& rootOut, std::wstring& folderOut)
{
    rootOut = SampleRootPath();
    if (rootOut.empty())
    {
        return false;
    }

    CleanupSample(rootOut);
    if (!::CreateDirectoryW(rootOut.c_str(), nullptr) &&
        ::GetLastError() != ERROR_ALREADY_EXISTS)
    {
        return false;
    }

    folderOut = rootOut + L"\\sample-folder";
    if (!::CreateDirectoryW(folderOut.c_str(), nullptr) &&
        ::GetLastError() != ERROR_ALREADY_EXISTS)
    {
        return false;
    }

    // 空样本文件（处理器按扩展名判断是否出现，内容无所谓）
    const wchar_t* sampleFiles[] =
    {
        L"sample.txt",
        L"sample.docx",
        L"sample.rar",
        L"sample.zip",
    };
    for (const wchar_t* file : sampleFiles)
    {
        const std::wstring path = rootOut + L"\\" + file;
        HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(handle);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 菜单树遍历
//
// menu        待遍历的菜单
// cm          对应的 IContextMenu（取动词用）
// parentPath  上级层级路径（一级为空；二级为一级文字）
// scene       当前样本场景位
// report      结果报告
// topGroup    一级分组名（进入子菜单时 = 一级项文字）
// ---------------------------------------------------------------------------
void WalkMenu(HMENU menu,
              IContextMenu* cm,
              UINT idFirst,
              const std::wstring& parentPath,
              unsigned int scene,
              ContextMenuReport& report,
              const std::wstring& topGroup)
{
    if (menu == nullptr || cm == nullptr)
    {
        return;
    }

    const int count = ::GetMenuItemCount(menu);
    for (int index = 0; index < count; ++index)
    {
        const UINT id = ::GetMenuItemID(menu, index);
        MENUITEMINFOW info = {};
        info.cbSize = sizeof(info);
        info.fMask  = MIIM_ID | MIIM_TYPE | MIIM_SUBMENU | MIIM_STATE;
        if (!::GetMenuItemInfoW(menu, index, TRUE, &info))
        {
            continue;
        }

        // 分隔线
        if ((info.fType & MFT_SEPARATOR) != 0)
        {
            ContextMenuItem item;
            item.isSeparator = true;
            item.text        = L"—";
            item.parentPath  = parentPath;
            item.scenes      = scene;
            const std::wstring key = L"sep:" + ToLower(topGroup.empty() ? parentPath : topGroup);
            FindOrAddGroup(report, key, topGroup.empty() ? L"其他" : topGroup, true)
                .items.push_back(std::move(item));
            continue;
        }

        // 取菜单文字
        wchar_t textBuffer[512] = {};
        ::GetMenuStringW(menu, index, textBuffer, ARRAYSIZE(textBuffer), MF_BYPOSITION);
        std::wstring text = StripAmpersand(textBuffer);

        // 命令偏移 = 命令 ID - idFirst（GetCommandString / InvokeCommand 都按偏移认）
        const UINT offset = (id != 0 && id != static_cast<UINT>(-1))
                                ? id - idFirst : static_cast<UINT>(-1);

        // 动词（GCS_VERBW）；取不到不影响条目展示
        std::wstring verb;
        if (offset != static_cast<UINT>(-1))
        {
            wchar_t verbBuffer[256] = {};
            HRESULT verbHr = cm->GetCommandString(static_cast<UINT_PTR>(offset),
                                                  GCS_VERBW, nullptr,
                                                  reinterpret_cast<LPSTR>(verbBuffer),
                                                  ARRAYSIZE(verbBuffer));
            if (SUCCEEDED(verbHr) && verbBuffer[0] != L'\0')
            {
                verb = verbBuffer;
            }
        }

        ContextMenuItem item;
        item.id         = std::to_wstring(id);
        item.text       = text;
        item.verb       = verb;
        item.parentPath = parentPath;
        item.scenes     = scene;
        item.commandOffset = (offset == static_cast<UINT>(-1)) ? -1 : static_cast<int>(offset);
        item.disabled   = (info.fState & MFS_GRAYED) != 0;

        const bool hasSubmenu = info.hSubMenu != nullptr;

        // 一级项：分组名 = 自己的文字；二级及以后：分组名沿用 topGroup
        std::wstring groupName = topGroup.empty() ? text : topGroup;
        if (groupName.empty())
        {
            groupName = L"其他";
        }
        const std::wstring sourceKey = L"menu:" + ToLower(groupName);
        ContextMenuGroup& group = FindOrAddGroup(report, sourceKey, groupName, true);
        group.items.push_back(item);

        // 子菜单：先让处理器填充（WM_INITMENUPOPUP），再递归
        if (hasSubmenu)
        {
            wil::com_ptr<IContextMenu2> cm2;
            if (SUCCEEDED(cm->QueryInterface(IID_PPV_ARGS(cm2.put()))) && cm2 != nullptr)
            {
                cm2->HandleMenuMsg(WM_INITMENUPOPUP,
                                   reinterpret_cast<WPARAM>(info.hSubMenu), 0);
            }

            WalkMenu(info.hSubMenu, cm, idFirst,
                     topGroup.empty() ? text : topGroup,
                     scene, report, groupName);
        }
    }
}

// 对单个 IContextMenu 采集菜单树，写进报告
void CollectOne(IContextMenu* cm, unsigned int scene, ContextMenuReport& report)
{
    if (cm == nullptr)
    {
        return;
    }

    const UINT idFirst = 1;   // 命令 ID 下限；偏移 = ID - idFirst

    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr)
    {
        return;
    }

    // 先按宽字符标准问；部分老旧扩展只认 ANSI，一期失败项留待二期兼容回退
    HRESULT hr = cm->QueryContextMenu(menu, 0, idFirst, 0x7FFF, CMF_NORMAL);
    if (SUCCEEDED(hr))
    {
        WalkMenu(menu, cm, idFirst, L"", scene, report, L"");
    }
    else
    {
        Logger::Warn(L"QueryContextMenu 失败：" + Logger::FormatHresult(hr));
    }

    ::DestroyMenu(menu);
}

// 文件 / 文件夹样本：按路径拿菜单对象
bool CollectForPath(const std::wstring& path, unsigned int scene,
                    ContextMenuReport& report)
{
    wil::com_ptr<IShellItem> item;
    HRESULT hr = ::SHCreateItemFromParsingName(path.c_str(), nullptr,
                                               IID_PPV_ARGS(item.put()));
    if (FAILED(hr) || item == nullptr)
    {
        return false;
    }

    wil::com_ptr<IContextMenu> cm;
    hr = item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(cm.put()));
    if (FAILED(hr) || cm == nullptr)
    {
        Logger::Warn(L"BindToHandler(BHID_SFUIObject) 失败：" + path +
                     L"，" + Logger::FormatHresult(hr));
        return false;
    }

    CollectOne(cm.get(), scene, report);
    return true;
}

// 背景样本：目录的 IShellFolder + 空 PIDL 拿背景菜单
bool CollectForBackground(const std::wstring& dir, ContextMenuReport& report)
{
    wil::com_ptr<IShellItem> item;
    if (FAILED(::SHCreateItemFromParsingName(dir.c_str(), nullptr,
                                              IID_PPV_ARGS(item.put()))) ||
        item == nullptr)
    {
        return false;
    }

    wil::com_ptr<IShellFolder> folder;
    HRESULT hr = item->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(folder.put()));
    if (FAILED(hr) || folder == nullptr)
    {
        return false;
    }

    wil::com_ptr<IContextMenu> cm;
    hr = folder->GetUIObjectOf(nullptr, 0, nullptr,
                               IID_IContextMenu, nullptr, cm.put_void());
    if (FAILED(hr) || cm == nullptr)
    {
        Logger::Warn(L"背景菜单 GetUIObjectOf 失败：" + dir +
                     L"，" + Logger::FormatHresult(hr));
        return false;
    }

    CollectOne(cm.get(), SceneBackground, report);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------

void CollectDynamicItems(ContextMenuReport& report)
{
    HRESULT initHr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needUninitialize = SUCCEEDED(initHr);

    try
    {
        std::wstring root, folder;
        if (!PrepareSample(root, folder))
        {
            Logger::Error(L"右键菜单样本目录准备失败");
            if (needUninitialize)
            {
                ::CoUninitialize();
            }
            return;
        }

        // 1) 文件样本（多种扩展名，覆盖不同文件处理器）
        const wchar_t* fileSamples[] =
        {
            L"sample.txt",
            L"sample.docx",
            L"sample.rar",
            L"sample.zip",
        };
        for (const wchar_t* file : fileSamples)
        {
            try
            {
                CollectForPath(root + L"\\" + file, SceneFile, report);
            }
            catch (...)
            {
                Logger::Warn(L"文件样本采集异常：" + std::wstring(file));
            }
        }

        // 2) 文件夹样本
        try
        {
            CollectForPath(folder, SceneFolder, report);
        }
        catch (...)
        {
            Logger::Warn(L"文件夹样本采集异常");
        }

        // 3) 背景样本
        try
        {
            CollectForBackground(root, report);
        }
        catch (...)
        {
            Logger::Warn(L"背景样本采集异常");
        }

        CleanupSample(root);
        report.collectedAt = NowTimeString();
        Logger::Info(L"右键菜单动态采集完成");
    }
    catch (...)
    {
        Logger::Error(L"右键菜单动态采集出现未知异常");
    }

    if (needUninitialize)
    {
        ::CoUninitialize();
    }
}

// ---------------------------------------------------------------------------
// 缓存 JSON 读写
// ---------------------------------------------------------------------------

using nlohmann::json;

json ItemToJson(const ContextMenuItem& item)
{
    json j;
    j["id"]           = ToUtf8(item.id);
    j["text"]         = ToUtf8(item.text);
    j["verb"]         = ToUtf8(item.verb);
    j["parentPath"]   = ToUtf8(item.parentPath);
    j["commandLine"]  = ToUtf8(item.commandLine);
    j["dllPath"]      = ToUtf8(item.dllPath);
    j["clsid"]        = ToUtf8(item.clsid);
    j["scenes"]       = item.scenes;
    j["commandOffset"]= item.commandOffset;
    j["isSeparator"]  = item.isSeparator;
    j["disabled"]     = item.disabled;
    j["failed"]       = item.failed;
    j["failReason"]   = ToUtf8(item.failReason);
    return j;
}

ContextMenuItem ItemFromJson(const json& j)
{
    ContextMenuItem item;
    auto str = [&j](const char* key) -> std::wstring
    {
        return j.contains(key) && j[key].is_string()
                   ? FromUtf8(j[key].get<std::string>())
                   : std::wstring();
    };
    item.id          = str("id");
    item.text        = str("text");
    item.verb        = str("verb");
    item.parentPath  = str("parentPath");
    item.commandLine = str("commandLine");
    item.dllPath     = str("dllPath");
    item.clsid       = str("clsid");
    item.scenes      = j.contains("scenes") ? j["scenes"].get<unsigned int>() : 0;
    item.commandOffset = j.contains("commandOffset") ? j["commandOffset"].get<int>() : -1;
    item.isSeparator = j.contains("isSeparator") && j["isSeparator"].get<bool>();
    item.disabled    = j.contains("disabled") && j["disabled"].get<bool>();
    item.failed      = j.contains("failed") && j["failed"].get<bool>();
    item.failReason  = str("failReason");
    return item;
}

void SaveCache(const ContextMenuReport& report)
{
    const std::wstring path = CacheFilePath();
    if (path.empty() || !EnsureDirectory(path))
    {
        return;
    }

    json root;
    root["collectedAt"] = ToUtf8(report.collectedAt);

    json groups = json::array();
    for (const ContextMenuGroup& group : report.groups)
    {
        json gj;
        gj["name"]      = ToUtf8(group.name);
        gj["sourceKey"] = ToUtf8(group.sourceKey);
        gj["isShellex"] = group.isShellex;

        json items = json::array();
        for (const ContextMenuItem& item : group.items)
        {
            items.push_back(ItemToJson(item));
        }
        gj["items"] = items;
        groups.push_back(gj);
    }
    root["groups"] = groups;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        return;
    }
    const std::string text = root.dump(2);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

bool LoadCache(ContextMenuReport& report)
{
    const std::wstring path = CacheFilePath();
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return false;
    }

    json root;
    try
    {
        in >> root;
    }
    catch (const std::exception& e)
    {
        Logger::Warn(std::wstring(L"菜单缓存 JSON 解析失败：") +
                     FromUtf8(e.what()));
        return false;
    }

    report.groups.clear();
    report.collectedAt = root.contains("collectedAt") && root["collectedAt"].is_string()
                             ? FromUtf8(root["collectedAt"].get<std::string>())
                             : std::wstring();

    if (root.contains("groups") && root["groups"].is_array())
    {
        for (const auto& gj : root["groups"])
        {
            ContextMenuGroup group;
            group.name      = gj.contains("name") && gj["name"].is_string()
                                  ? FromUtf8(gj["name"].get<std::string>()) : std::wstring();
            group.sourceKey = gj.contains("sourceKey") && gj["sourceKey"].is_string()
                                  ? FromUtf8(gj["sourceKey"].get<std::string>()) : std::wstring();
            group.isShellex = gj.contains("isShellex") && gj["isShellex"].get<bool>();

            if (gj.contains("items") && gj["items"].is_array())
            {
                for (const auto& ij : gj["items"])
                {
                    group.items.push_back(ItemFromJson(ij));
                }
            }
            report.groups.push_back(std::move(group));
        }
    }
    return true;
}

} // namespace shell
