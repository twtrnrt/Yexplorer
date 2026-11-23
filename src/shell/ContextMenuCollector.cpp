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
#include "shell/ContextMenuEnumerator.h"

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
        // 多选样本：两个同类型文件，放在同一目录（多选菜单要求同父目录）
        L"multi-a.txt",
        L"multi-b.txt",
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
// 把一条动态菜单项并进报告
//
// 去重钥匙 = 分组 + 层级 + 菜文字：同一个样本采到、或不同样本采到的同一条
// （文字一样、层级一样、分组一样）只留一条，场景位取并集。
// 这样 .txt/.docx/.rar/.zip 各采一遍不会再出现 4 份「打开」。
// ---------------------------------------------------------------------------
void AddDynamicItem(ContextMenuReport& report,
                    const std::wstring& groupName,
                    const ContextMenuItem& item)
{
    const std::wstring name = groupName.empty() ? L"其他" : groupName;
    const std::wstring sourceKey = L"menu:" + ToLower(name);
    ContextMenuGroup& group = FindOrAddGroup(report, sourceKey, name, true);

    for (ContextMenuItem& existing : group.items)
    {
        if (existing.isSeparator || item.isSeparator)
        {
            continue;
        }
        if (existing.parentPath == item.parentPath && existing.text == item.text)
        {
            // 已存在：并场景位；有一样本可用就不算灰态；补动词 / 偏移
            existing.scenes |= item.scenes;
            existing.disabled = existing.disabled && item.disabled;
            if (existing.verb.empty() && !item.verb.empty())
            {
                existing.verb = item.verb;
            }
            if (existing.commandOffset < 0 && item.commandOffset >= 0)
            {
                existing.commandOffset = item.commandOffset;
            }
            existing.fromExtension = true;
            existing.isSystemBuiltin = existing.isSystemBuiltin || item.isSystemBuiltin;
            return;
        }
    }

    group.items.push_back(item);
}

// ---------------------------------------------------------------------------
// 菜单树遍历（分隔线一律跳过，不生成任何条目）
//
// menu        待遍历的菜单
// cm          对应的 IContextMenu（取动词用）
// idFirst     命令 ID 下限（前缀一个，偏移 = ID - idFirst）
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

        // 分隔线：直接跳过（用户要求不展示）
        if ((info.fType & MFT_SEPARATOR) != 0)
        {
            continue;
        }

        // 取菜单文字
        wchar_t textBuffer[512] = {};
        ::GetMenuStringW(menu, index, textBuffer, ARRAYSIZE(textBuffer), MF_BYPOSITION);
        std::wstring text = StripAmpersand(textBuffer);
        if (text.empty())
        {
            continue;   // 取不到文字的空项没有展示价值
        }

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
        item.fromExtension = true;
        item.isSystemBuiltin = IsSystemBuiltinItem(text, verb);

        // 一级项：分组名 = 自己的文字；二级及以后：分组名沿用 topGroup
        const std::wstring groupName = topGroup.empty() ? text : topGroup;
        AddDynamicItem(report, groupName, item);

        // 子菜单：先让处理器填充（WM_INITMENUPOPUP），再递归
        if (info.hSubMenu != nullptr)
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

// ---------------------------------------------------------------------------
// 多选样本：同一目录下两个文件
//
// 多选菜单只能从"父 IShellFolder + 两个子 PIDL"拿：
//   SHParseDisplayName 得到绝对 PIDL → SHBindToParent 拆出父文件夹和末级子 PIDL
//   → GetUIObjectOf(cidl=2) 拿多选合并菜单
// ---------------------------------------------------------------------------
bool CollectForMultiSelect(const std::wstring& path1, const std::wstring& path2,
                           ContextMenuReport& report)
{
    PIDLIST_ABSOLUTE pidl1 = nullptr;
    PIDLIST_ABSOLUTE pidl2 = nullptr;
    SFGAOF attrs = 0;

    bool ok = false;

    if (SUCCEEDED(::SHParseDisplayName(path1.c_str(), nullptr, &pidl1, 0, &attrs)) &&
        pidl1 != nullptr &&
        SUCCEEDED(::SHParseDisplayName(path2.c_str(), nullptr, &pidl2, 0, &attrs)) &&
        pidl2 != nullptr)
    {
        wil::com_ptr<IShellFolder> folder;
        PCUITEMID_CHILD child1 = nullptr;
        PCUITEMID_CHILD child2 = nullptr;

        HRESULT hr = ::SHBindToParent(pidl1, IID_IShellFolder,
                                      folder.put_void(), &child1);
        if (SUCCEEDED(hr) && folder != nullptr && child1 != nullptr)
        {
            wil::com_ptr<IShellFolder> folder2;
            hr = ::SHBindToParent(pidl2, IID_IShellFolder,
                                  folder2.put_void(), &child2);

            if (SUCCEEDED(hr) && child2 != nullptr)
            {
                PCUITEMID_CHILD children[2] = { child1, child2 };

                wil::com_ptr<IContextMenu> cm;
                hr = folder->GetUIObjectOf(nullptr, 2, children,
                                           IID_IContextMenu, nullptr, cm.put_void());
                if (SUCCEEDED(hr) && cm != nullptr)
                {
                    CollectOne(cm.get(), SceneMultiple, report);
                    ok = true;
                }
                else
                {
                    Logger::Warn(L"多选菜单 GetUIObjectOf 失败：" +
                                 Logger::FormatHresult(hr));
                }
            }
            else
            {
                Logger::Warn(L"多选样本第二个 PIDL 拆分失败：" +
                             Logger::FormatHresult(hr));
            }
        }
        else
        {
            Logger::Warn(L"多选样本 PIDL 拆分失败：" + Logger::FormatHresult(hr));
        }
    }

    if (pidl1 != nullptr)
    {
        ::CoTaskMemFree(pidl1);
    }
    if (pidl2 != nullptr)
    {
        ::CoTaskMemFree(pidl2);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// 按软件归并分组
//
// 背景：菜单是"合并查询"拿到的，接口不告诉每一项属于哪个 DLL，所以只能按
// 一级菜单文字分组。同一家软件常在菜单里出现多个一级文字（如 "WPS" 和
// "上传到 WPS"），这里用处理器清单里的软件名做"软匹配"把它们并到一组。
//
// 匹配规则：从每个处理器的显示名（DLL 版本资源的产品名优先）取两个 token：
//   ① 整个名字去掉空格小写；② 第一个单词（长度 >= 3 且不是厂商通用词）。
// 分组名（去掉空格小写）包含任一 token，就归到这个软件名下。
// 这是启发式，可能偶尔归错或归得过宽；只影响整齐度，不影响数据。
// ---------------------------------------------------------------------------

// 去掉空格和 '&'、转小写
std::wstring NormalizeGroupName(const std::wstring& text)
{
    std::wstring out;
    out.reserve(text.size());
    for (wchar_t ch : text)
    {
        if (ch == L'&' || ch == L' ' || ch == L'\t')
        {
            continue;
        }
        out.push_back(static_cast<wchar_t>(::towlower(ch)));
    }
    return out;
}

// 厂商通用词，做 token 会造成大面积误并，直接不用
bool IsGenericToken(const std::wstring& token)
{
    static const wchar_t* generics[] =
    {
        L"microsoft", L"windows", L"system", L"office", L"intel", L"adobe",
        L"google", L"apple", L"tencent", L"alibaba", L"baidu", L"huawei",
    };
    for (const wchar_t* one : generics)
    {
        if (token == one)
        {
            return true;
        }
    }
    return false;
}

struct VendorName
{
    std::wstring display;                 // 归并后的组名（用处理器的软件名）
    std::vector<std::wstring> tokens;     // 用于匹配的小写 token
};

std::vector<VendorName> BuildVendors(
    const std::vector<EnumeratedHandler>& handlers)
{
    std::vector<VendorName> vendors;

    for (const EnumeratedHandler& handler : handlers)
    {
        const std::wstring display = handler.displayName;
        if (display.empty())
        {
            continue;
        }

        // 已经有了就不重复建
        const std::wstring normalized = NormalizeGroupName(display);
        bool exists = false;
        for (const VendorName& vendor : vendors)
        {
            if (NormalizeGroupName(vendor.display) == normalized)
            {
                exists = true;
                break;
            }
        }
        if (exists)
        {
            continue;
        }

        VendorName vendor;
        vendor.display = display;

        if (!normalized.empty())
        {
            vendor.tokens.push_back(normalized);
        }

        // 第一个单词：按空格 / 连字符切开，取第一段
        std::wstring firstWord;
        for (wchar_t ch : display)
        {
            if (ch == L' ' || ch == L'\t')
            {
                break;
            }
            firstWord.push_back(static_cast<wchar_t>(::towlower(ch)));
        }
        if (firstWord.size() >= 3 && firstWord != normalized &&
            !IsGenericToken(firstWord))
        {
            vendor.tokens.push_back(firstWord);
        }

        if (!vendor.tokens.empty())
        {
            vendors.push_back(std::move(vendor));
        }
    }

    return vendors;
}

// 把 src 的条目并进 target，条目按「层级 + 文字」去重
void MergeItemInto(ContextMenuGroup& target, const ContextMenuItem& src)
{
    for (ContextMenuItem& existing : target.items)
    {
        if (existing.parentPath == src.parentPath && existing.text == src.text)
        {
            existing.scenes |= src.scenes;
            existing.disabled = existing.disabled && src.disabled;
            existing.fromRegistry = existing.fromRegistry || src.fromRegistry;
            existing.fromExtension = existing.fromExtension || src.fromExtension;
            existing.isSystemBuiltin = existing.isSystemBuiltin || src.isSystemBuiltin;
            if (existing.verb.empty())
            {
                existing.verb = src.verb;
            }
            if (existing.commandLine.empty())
            {
                existing.commandLine = src.commandLine;
            }
            if (existing.commandOffset < 0)
            {
                existing.commandOffset = src.commandOffset;
            }
            return;
        }
    }
    target.items.push_back(src);
}

// 全局归并：先按软件名归档，再把同名组合并、条目去重、去掉空组
void MergeGroups(ContextMenuReport& report,
                 const std::vector<EnumeratedHandler>& handlers)
{
    const std::vector<VendorName> vendors = BuildVendors(handlers);

    std::vector<ContextMenuGroup> merged;

    for (const ContextMenuGroup& group : report.groups)
    {
        if (group.items.empty())
        {
            continue;   // 空组不展示
        }

        // 目标组名：默认原名；能匹配到软件名就用软件名
        std::wstring targetName = group.name;
        const std::wstring normalizedGroup = NormalizeGroupName(group.name);
        if (!normalizedGroup.empty())
        {
            for (const VendorName& vendor : vendors)
            {
                bool matched = false;
                for (const std::wstring& token : vendor.tokens)
                {
                    if (!token.empty() && normalizedGroup.find(token) != std::wstring::npos)
                    {
                        matched = true;
                        break;
                    }
                }
                if (matched)
                {
                    targetName = vendor.display;
                    break;
                }
            }
        }

        const std::wstring targetKey = L"merged:" + NormalizeGroupName(targetName);

        // 找已存在的同名组
        ContextMenuGroup* existing = nullptr;
        for (ContextMenuGroup& candidate : merged)
        {
            if (candidate.sourceKey == targetKey)
            {
                existing = &candidate;
                break;
            }
        }

        if (existing == nullptr)
        {
            ContextMenuGroup fresh;
            fresh.sourceKey = targetKey;
            fresh.name      = targetName;
            fresh.isShellex = group.isShellex;
            merged.push_back(std::move(fresh));
            existing = &merged.back();
        }

        for (const ContextMenuItem& item : group.items)
        {
            MergeItemInto(*existing, item);
        }
        existing->isShellex = existing->isShellex || group.isShellex;
    }

    // 组级系统判定：如「新建」「发送到」整组标系统项
    for (ContextMenuGroup& group : merged)
    {
        if (IsSystemBuiltinGroup(group.name))
        {
            for (ContextMenuItem& item : group.items)
            {
                item.isSystemBuiltin = true;
            }
        }
    }

    report.groups = std::move(merged);
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

        // 4) 多选样本（同目录两个文件）
        try
        {
            CollectForMultiSelect(root + L"\\multi-a.txt",
                                  root + L"\\multi-b.txt", report);
        }
        catch (...)
        {
            Logger::Warn(L"多选样本采集异常");
        }

        CleanupSample(root);

        // 5) 按软件归并 + 条目去重（消掉同族多组、重复项）
        const std::vector<EnumeratedHandler> handlers = EnumerateShellexHandlers();
        MergeGroups(report, handlers);

        report.collectedAt = NowTimeString();
        Logger::Info(L"右键菜单动态采集完成：分组 " +
                     std::to_wstring(report.groups.size()) +
                     L"，处理器 " + std::to_wstring(handlers.size()));
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

// 缓存格式版本：整理规则一变就换号，老缓存自动作废重采
const int kCacheVersion = 2;

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
    j["fromRegistry"] = item.fromRegistry;
    j["fromExtension"]= item.fromExtension;
    j["isSystemBuiltin"] = item.isSystemBuiltin;
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
    item.fromRegistry = j.contains("fromRegistry") && j["fromRegistry"].get<bool>();
    item.fromExtension = j.contains("fromExtension") && j["fromExtension"].get<bool>();
    item.isSystemBuiltin = j.contains("isSystemBuiltin") && j["isSystemBuiltin"].get<bool>();
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
    root["version"]     = kCacheVersion;
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

    // 老格式缓存（没有 version 或版本号不符）直接作废，让上层重采
    const int version = root.contains("version") && root["version"].is_number()
                            ? root["version"].get<int>() : 0;
    if (version != kCacheVersion)
    {
        Logger::Info(L"菜单缓存版本不匹配，作废重采");
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
