// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/MenuCensus.cpp
//
// 普查实现：注册表层枚举扩展名 → 子进程逐批真实采集 → 归并 → 分簇 → 落盘。
// 说明见 MenuCensus.h。
// ============================================================================

#include "shell/MenuCensus.h"

#include "core/Logger.h"

#include <wil/com.h>

#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#pragma comment(lib, "shlwapi.lib")

namespace shell
{

namespace
{

// 每批扩展名数量：一批一个子进程，崩了只丢这一批
const int kBatchSize = 40;

// 每个扩展名采集超时（毫秒），超时算失败（放在子进程里也没法强杀，只能记日志）
const int kExtTimeoutMs = 0;   // 暂不使用（子进程整体受父进程等待超时约束）

using nlohmann::json;

// ---------------------------------------------------------------------------
// 基础工具
// ---------------------------------------------------------------------------

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty()) return std::string();
    int length = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                       static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return std::string();
    std::string result(static_cast<size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                          static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(const std::string& text)
{
    if (text.empty()) return std::wstring();
    int length = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                       static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return std::wstring();
    std::wstring result(static_cast<size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                          static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::wstring AppDataDir()
{
    LPWSTR appData = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) || !appData)
    {
        return std::wstring();
    }
    std::wstring path = appData;
    ::CoTaskMemFree(appData);
    return path;
}

std::wstring AppDataSubDir(const wchar_t* name)
{
    const std::wstring appData = AppDataDir();
    if (appData.empty()) return std::wstring();
    const std::wstring dir = appData + L"\\FileExplorer\\" + name;
    ::SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

std::wstring NowTime()
{
    SYSTEMTIME st = {};
    ::GetLocalTime(&st);
    wchar_t buffer[32] = {};
    ::swprintf_s(buffer, L"%04d-%02d-%02d %02d:%02d:%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

std::wstring StripAmpersand(std::wstring text)
{
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == L'&')
        {
            if (i + 1 < text.size() && text[i + 1] == L'&') { out.push_back(L'&'); ++i; continue; }
            continue;
        }
        out.push_back(text[i]);
    }
    // 去掉首尾空白
    while (!out.empty() && std::iswspace(out.front())) out.erase(out.begin());
    while (!out.empty() && std::iswspace(out.back())) out.pop_back();
    return out;
}

// ---------------------------------------------------------------------------
// 品牌识别：把一级菜单文字归到"软件"上
// ---------------------------------------------------------------------------

std::wstring BrandOfText(const std::wstring& text)
{
    if (text.empty()) return L"系统";

    struct Rule { const wchar_t* keyword; const wchar_t* brand; };
    static const Rule rules[] = {
        { L"WinRAR",            L"WinRAR"      },
        { L"7-Zip",             L"7-Zip"       },
        { L"TortoiseSVN",       L"TortoiseSVN" },
        { L"Subversion",        L"TortoiseSVN" },
        { L"SVN",               L"TortoiseSVN" },
        { L"WPS",               L"WPS"         },
        { L"ima",               L"ima"         },
        { L"Tailscale",         L"Tailscale"   },
        { L"ChatGPT",           L"ChatGPT"     },
        { L"Git",               L"Git"         },
        { L"Visual Studio",     L"Visual Studio" },
        { L"批量",              L"批量工具"     },
        { L"压缩(zipped)",      L"系统"         },
    };

    for (const Rule& rule : rules)
    {
        if (text.find(rule.keyword) != std::wstring::npos)
        {
            return rule.brand;
        }
    }
    return L"系统";
}

// ---------------------------------------------------------------------------
// 文字模板化：把样本文件名/目录名替换成占位符，避免不同样本造成"假差异"
// ---------------------------------------------------------------------------

struct Normalizer
{
    std::wstring dirName;
    std::wstring fileName;
    std::wstring stem;

    std::wstring Apply(std::wstring text) const
    {
        auto replaceAll = [](std::wstring s, const std::wstring& from, const std::wstring& to)
        {
            if (from.empty()) return s;
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::wstring::npos)
            {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
            return s;
        };
        text = replaceAll(std::move(text), dirName, L"<目录>");
        text = replaceAll(std::move(text), fileName, L"<文件名>");
        text = replaceAll(std::move(text), stem, L"<文件名>");
        return text;
    }
};

// ---------------------------------------------------------------------------
// 图标：把菜单项自带的位图（hbmpItem）存成 16×16 BMP，按身份去重
// ---------------------------------------------------------------------------

std::wstring HashKey(const std::wstring& text)
{
    unsigned int hash = 2166136261u;
    for (wchar_t c : text)
    {
        hash ^= static_cast<unsigned int>(c);
        hash *= 16777619u;
    }
    wchar_t buffer[16] = {};
    ::swprintf_s(buffer, L"%08x", hash);
    return buffer;
}

bool SaveBitmapAsBmp(HBITMAP bitmap, int size, const std::wstring& path)
{
    BITMAP info = {};
    if (::GetObjectW(bitmap, sizeof(info), &info) != sizeof(info) || info.bmWidth <= 0)
    {
        return false;
    }

    BITMAPINFO dibInfo = {};
    dibInfo.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    dibInfo.bmiHeader.biWidth       = size;
    dibInfo.bmiHeader.biHeight      = size;
    dibInfo.bmiHeader.biPlanes      = 1;
    dibInfo.bmiHeader.biBitCount    = 32;
    dibInfo.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = ::GetDC(nullptr);
    HBITMAP canvas = ::CreateDIBSection(screen, &dibInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    if (!canvas || !bits) return false;

    HDC memDc = ::CreateCompatibleDC(nullptr);
    HGDIOBJ oldCanvas = ::SelectObject(memDc, canvas);
    RECT full = { 0, 0, size, size };
    ::FillRect(memDc, &full, static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH)));

    HDC srcDc = ::CreateCompatibleDC(nullptr);
    HGDIOBJ oldSrc = ::SelectObject(srcDc, bitmap);
    ::SetStretchBltMode(memDc, HALFTONE);
    ::StretchBlt(memDc, 0, 0, size, size, srcDc, 0, 0, info.bmWidth, info.bmHeight, SRCCOPY);
    ::SelectObject(srcDc, oldSrc);
    ::DeleteDC(srcDc);

    // 检查是否全白（白图等于没有图标）
    const unsigned char* pixels = static_cast<const unsigned char*>(bits);
    bool blank = true;
    for (int i = 0; i < size * size * 4; i += 4)
    {
        if (pixels[i] != 0xFF || pixels[i + 1] != 0xFF || pixels[i + 2] != 0xFF) { blank = false; break; }
    }

    const int stride = size * 4;
    const DWORD imageSize = static_cast<DWORD>(stride) * static_cast<DWORD>(size);

    BITMAPFILEHEADER fileHeader = {};
    fileHeader.bfType    = 0x4D42;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize    = fileHeader.bfOffBits + imageSize;

    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        ::WriteFile(file, &fileHeader, sizeof(fileHeader), &written, nullptr);
        ::WriteFile(file, &dibInfo.bmiHeader, sizeof(BITMAPINFOHEADER), &written, nullptr);
        ::WriteFile(file, bits, imageSize, &written, nullptr);
        ::CloseHandle(file);
    }

    ::SelectObject(memDc, oldCanvas);
    ::DeleteDC(memDc);
    ::DeleteObject(canvas);
    return !blank;
}

std::wstring SaveItemIcon(HBITMAP bitmap, const std::wstring& pathKey)
{
    if (bitmap == nullptr || bitmap == HBMMENU_CALLBACK)
    {
        return std::wstring();
    }
    const std::wstring dir = IconDir();
    if (dir.empty()) return std::wstring();

    const std::wstring name = L"c" + HashKey(pathKey) + L".bmp";
    const std::wstring path = dir + L"\\" + name;
    if (::PathFileExistsW(path.c_str())) return name;

    if (!SaveBitmapAsBmp(bitmap, 16, path)) return std::wstring();
    return name;
}

} // namespace

// ---------------------------------------------------------------------------
// 对外：路径
// ---------------------------------------------------------------------------

std::wstring IconDir()
{
    return AppDataSubDir(L"menu-icons");
}

std::wstring CensusFilePath()
{
    const std::wstring appData = AppDataDir();
    if (appData.empty()) return std::wstring();
    return appData + L"\\FileExplorer\\menu-census.json";
}

int CensusReport::BrokenCount() const
{
    int count = 0;
    for (const CensusExtension& item : extensions)
    {
        if (!item.ok) ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------
// 采集一个扩展名（在子进程里跑）
// -------------------------------------------------------------------------------------------------------------------------------------------
namespace
{

// 递归遍历菜单，收集条目（含品牌继承 / 身份键 / 图标）
void WalkCensusMenu(HMENU menu,
                    IContextMenu3* cm3,
                    int level,
                    const Normalizer& normalizer,
                    std::vector<std::wstring>& textPath,
                    std::wstring inheritedBrand,
                    std::vector<CensusItem>& out)
{
    const int count = ::GetMenuItemCount(menu);
    for (int index = 0; index < count; ++index)
    {
        MENUITEMINFOW mi = {};
        mi.cbSize = sizeof(mi);
        mi.fMask  = MIIM_ID | MIIM_FTYPE | MIIM_SUBMENU | MIIM_BITMAP | MIIM_DATA | MIIM_STATE;
        if (!::GetMenuItemInfoW(menu, index, TRUE, &mi))
        {
            continue;
        }

        CensusItem item;
        item.level = level;

        if ((mi.fType & MFT_SEPARATOR) != 0)
        {
            item.separator = true;
            out.push_back(item);
            continue;
        }

        wchar_t buffer[512] = {};
        ::GetMenuStringW(menu, index, buffer, ARRAYSIZE(buffer), MF_BYPOSITION);
        item.text = StripAmpersand(normalizer.Apply(buffer));
        item.disabled = (mi.fState & MFS_GRAYED) != 0;

        // 品牌：一级项自己判断，子项继承父项
        std::wstring brand = (level == 0) ? BrandOfText(item.text) : inheritedBrand;
        if (brand.empty()) brand = L"系统";
        item.brand = brand;

        if (cm3 != nullptr)
        {
            const UINT idFirst = 1;
            if (mi.wID > idFirst)
            {
                wchar_t verbBuffer[256] = {};
                HRESULT hr = cm3->GetCommandString(mi.wID - idFirst, GCS_VERBW, nullptr,
                                                   reinterpret_cast<LPSTR>(verbBuffer),
                                                   ARRAYSIZE(verbBuffer));
                if (SUCCEEDED(hr)) item.verb = verbBuffer;
            }
        }

        // 身份键：品牌 + 动词 + 各级模板文字
        std::wstring keyPath;
        for (const std::wstring& part : textPath) { keyPath += part; keyPath.push_back(L'\x1f'); }
        keyPath += item.text;
        item.pathKey = brand + L"|" + item.verb + L"|" + keyPath;

        // 图标：只取菜单项自带的位图
        if (mi.hbmpItem != nullptr)
        {
            item.iconFile = SaveItemIcon(mi.hbmpItem, item.pathKey);
        }

        out.push_back(item);

        if (mi.hSubMenu != nullptr)
        {
            if (cm3 != nullptr)
            {
                cm3->HandleMenuMsg2(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(mi.hSubMenu),
                                    MAKELPARAM(index, FALSE), nullptr);
            }
            textPath.push_back(item.text);
            WalkCensusMenu(mi.hSubMenu, cm3, level + 1, normalizer, textPath, brand, out);
            textPath.pop_back();
        }
    }
}

// 对给定的 IContextMenu 采一份（内部公用）
void CollectFromContextMenu(IContextMenu* cm,
                            IContextMenu3* cm3,
                            HWND host,
                            const Normalizer& normalizer,
                            std::vector<CensusItem>& out)
{
    (void)host;   // 采集阶段不需要 owner 窗口（某些扩展会用它定位，当前留空即可）
    const UINT idFirst = 1;
    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) return;

    if (SUCCEEDED(cm->QueryContextMenu(menu, 0, idFirst, 0x7FFF, CMF_NORMAL)))
    {
        std::vector<std::wstring> textPath;
        WalkCensusMenu(menu, cm3, 0, normalizer, textPath, L"", out);
    }
    ::DestroyMenu(menu);
}

HWND CreateProbeHost()
{
    const wchar_t* className = L"FileExplorerCensusHost";
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = ::DefWindowProcW;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    ::RegisterClassExW(&wc);
    return ::CreateWindowExW(0, className, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                             ::GetModuleHandleW(nullptr), nullptr);
}

} // namespace

bool CollectExtension(const std::wstring& dir, const std::wstring& ext, CensusExtension& out)
{
    out.ext = ext;
    out.items.clear();

    const std::wstring fileName = L"sample" + ext;
    const std::wstring path = dir + L"\\" + fileName;

    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        out.ok = false;
        out.failReason = L"创建样本文件失败";
        return false;
    }
    ::CloseHandle(file);

    Normalizer normalizer;
    normalizer.dirName = dir.substr(dir.find_last_of(L'\\') + 1);
    normalizer.fileName = fileName;
    normalizer.stem = L"sample";

    wil::com_ptr<IShellItem> item;
    if (FAILED(::SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.put()))))
    {
        out.failReason = L"解析路径失败";
        ::DeleteFileW(path.c_str());
        return false;
    }

    wil::com_ptr<IContextMenu> cm;
    if (FAILED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(cm.put()))))
    {
        out.failReason = L"拿菜单对象失败";
        ::DeleteFileW(path.c_str());
        return false;
    }

    wil::com_ptr<IContextMenu3> cm3;
    cm->QueryInterface(IID_PPV_ARGS(cm3.put()));

    HWND host = CreateProbeHost();
    CollectFromContextMenu(cm.get(), cm3.get(), host, normalizer, out.items);
    if (host != nullptr) ::DestroyWindow(host);

    ::DeleteFileW(path.c_str());

    out.ok = !out.items.empty();
    if (!out.ok) out.failReason = L"菜单为空";
    return out.ok;
}

// ---------------------------------------------------------------------------
// 非文件场景：多选 / 文件夹 / 空白区
// -------------------------------------------------------------------------------------------------------------------------------------------
namespace
{

std::wstring SampleDir()
{
    wchar_t tempPath[MAX_PATH] = {};
    if (::GetTempPathW(ARRAYSIZE(tempPath), tempPath) == 0) return std::wstring();
    const std::wstring dir = std::wstring(tempPath) + L"FileExplorerCensusSample";
    ::SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

bool CollectFolderScene(const std::wstring& dir, CensusScene& out)
{
    Normalizer normalizer;
    normalizer.dirName = dir.substr(dir.find_last_of(L'\\') + 1);

    wil::com_ptr<IShellItem> item;
    if (FAILED(::SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(item.put())))
        || item == nullptr)
    {
        out.failReason = L"解析目录失败";
        return false;
    }

    wil::com_ptr<IContextMenu> cm;
    if (FAILED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(cm.put()))))
    {
        out.failReason = L"拿菜单对象失败";
        return false;
    }

    wil::com_ptr<IContextMenu3> cm3;
    cm->QueryInterface(IID_PPV_ARGS(cm3.put()));
    HWND host = CreateProbeHost();
    CollectFromContextMenu(cm.get(), cm3.get(), host, normalizer, out.items);
    if (host) ::DestroyWindow(host);
    return !out.items.empty();
}

bool CollectBackgroundScene(const std::wstring& dir, CensusScene& out)
{
    Normalizer normalizer;
    normalizer.dirName = dir.substr(dir.find_last_of(L'\\') + 1);

    wil::com_ptr<IShellItem> item;
    if (FAILED(::SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(item.put())))
        || item == nullptr)
    {
        out.failReason = L"解析目录失败";
        return false;
    }

    wil::com_ptr<IShellFolder> folder;
    if (FAILED(item->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(folder.put())))
        || folder == nullptr)
    {
        out.failReason = L"拿文件夹对象失败";
        return false;
    }

    HWND host = CreateProbeHost();
    wil::com_ptr<IContextMenu> cm;
    wil::com_ptr<IShellView> view;
    if (SUCCEEDED(folder->CreateViewObject(host, IID_PPV_ARGS(view.put()))) && view != nullptr)
    {
        view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(cm.put()));
    }
    if (cm == nullptr)
    {
        folder->GetUIObjectOf(nullptr, 0, nullptr, IID_IContextMenu, nullptr, cm.put_void());
    }
    if (cm == nullptr)
    {
        if (host) ::DestroyWindow(host);
        out.failReason = L"拿空白区菜单失败";
        return false;
    }

    wil::com_ptr<IContextMenu3> cm3;
    cm->QueryInterface(IID_PPV_ARGS(cm3.put()));
    CollectFromContextMenu(cm.get(), cm3.get(), host, normalizer, out.items);
    if (host) ::DestroyWindow(host);
    return !out.items.empty();
}

bool CollectMultiScene(const std::wstring& dir, CensusScene& out)
{
    Normalizer normalizer;
    normalizer.dirName = dir.substr(dir.find_last_of(L'\\') + 1);
    normalizer.fileName = L"multi.txt";
    normalizer.stem = L"multi";

    const std::wstring p1 = dir + L"\\multi.txt";
    const std::wstring p2 = dir + L"\\multi2.txt";
    for (const std::wstring& p : { p1, p2 })
    {
        HANDLE h = ::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
    }

    PIDLIST_ABSOLUTE pidl1 = nullptr;
    PIDLIST_ABSOLUTE pidl2 = nullptr;
    SFGAOF attrs = 0;
    auto cleanup = [&]()
    {
        if (pidl1) ::CoTaskMemFree(pidl1);
        if (pidl2) ::CoTaskMemFree(pidl2);
        ::DeleteFileW(p1.c_str());
        ::DeleteFileW(p2.c_str());
    };

    if (FAILED(::SHParseDisplayName(p1.c_str(), nullptr, &pidl1, 0, &attrs)) ||
        FAILED(::SHParseDisplayName(p2.c_str(), nullptr, &pidl2, 0, &attrs)))
    {
        out.failReason = L"解析样本失败";
        cleanup();
        return false;
    }

    wil::com_ptr<IShellFolder> folder;
    PCUITEMID_CHILD child1 = nullptr;
    if (FAILED(::SHBindToParent(pidl1, IID_IShellFolder, folder.put_void(), &child1)) || !child1)
    {
        out.failReason = L"取父文件夹失败";
        cleanup();
        return false;
    }

    wil::com_ptr<IShellFolder> folder2;
    PCUITEMID_CHILD child2 = nullptr;
    if (FAILED(::SHBindToParent(pidl2, IID_IShellFolder, folder2.put_void(), &child2)) || !child2)
    {
        out.failReason = L"取第二个子项失败";
        cleanup();
        return false;
    }

    PCUITEMID_CHILD children[2] = { child1, child2 };
    wil::com_ptr<IContextMenu> cm;
    if (FAILED(folder->GetUIObjectOf(nullptr, 2, children, IID_IContextMenu,
                                     nullptr, cm.put_void())) || cm == nullptr)
    {
        out.failReason = L"拿多选菜单失败";
        cleanup();
        return false;
    }

    wil::com_ptr<IContextMenu3> cm3;
    cm->QueryInterface(IID_PPV_ARGS(cm3.put()));
    HWND host = CreateProbeHost();
    CollectFromContextMenu(cm.get(), cm3.get(), host, normalizer, out.items);
    if (host) ::DestroyWindow(host);

    cleanup();
    return !out.items.empty();
}

} // namespace

void CollectNonFileScenes(std::vector<CensusScene>& outScenes)
{
    outScenes.clear();
    const std::wstring dir = SampleDir();
    if (dir.empty()) return;

    // 文件夹：造个空子目录
    const std::wstring folder = dir + L"\\sample-folder";
    ::SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);

    CensusScene folderScene;
    folderScene.scene = 1u << 1;   // SceneFolder
    folderScene.ok = CollectFolderScene(folder, folderScene);
    outScenes.push_back(std::move(folderScene));

    CensusScene bgScene;
    bgScene.scene = 1u << 3;       // SceneBackground
    bgScene.ok = CollectBackgroundScene(dir, bgScene);
    outScenes.push_back(std::move(bgScene));

    CensusScene multiScene;
    multiScene.scene = 1u << 2;    // SceneMultiple
    multiScene.ok = CollectMultiScene(dir, multiScene);
    outScenes.push_back(std::move(multiScene));
}

// ---------------------------------------------------------------------------
// 分簇：同一品牌 + 同一签名（条目序列完全一致）→ 一个簇
// -------------------------------------------------------------------------------------------------------------------------------------------
namespace
{

std::wstring BrandSignature(const std::vector<CensusItem>& items, const std::wstring& brand)
{
    std::wstring sig;
    for (const CensusItem& item : items)
    {
        if (item.brand != brand) continue;
        sig += std::to_wstring(item.level);
        sig.push_back(L':');
        sig += item.separator ? L"<SEP>" : item.text;
        if (item.disabled) sig += L"[灰]";
        sig.push_back(L'\x1f');
    }
    return sig;
}

} // namespace

void BuildClusters(CensusReport& report)
{
    report.clusters.clear();

    struct Bucket
    {
        std::wstring signature;
        std::vector<std::wstring> exts;
        std::vector<CensusItem> items;
        std::wstring sampleExt;
    };

    // brand → signature → bucket
    std::vector<std::pair<std::wstring, std::vector<Bucket>>> brands;

    auto findBrand = [&brands](const std::wstring& brand) -> std::vector<Bucket>&
    {
        for (auto& entry : brands)
        {
            if (entry.first == brand) return entry.second;
        }
        brands.push_back({ brand, {} });
        return brands.back().second;
    };

    std::set<std::wstring> knownBrands;
    for (const CensusExtension& ext : report.extensions)
    {
        if (!ext.ok) continue;
        for (const CensusItem& item : ext.items)
        {
            if (!item.separator) knownBrands.insert(item.brand);
        }
    }

    for (const CensusExtension& ext : report.extensions)
    {
        if (!ext.ok) continue;
        for (const std::wstring& brand : knownBrands)
        {
            if (brand == L"系统") continue;   // 系统品牌走 BuildMenuGroups（按出现范围分组，不分软件簇）
            const std::wstring signature = BrandSignature(ext.items, brand);
            if (signature.empty()) continue;   // 这一家在这类文件上没菜单

            std::vector<Bucket>& buckets = findBrand(brand);
            bool matched = false;
            for (Bucket& bucket : buckets)
            {
                if (bucket.signature == signature)
                {
                    bucket.exts.push_back(ext.ext);
                    matched = true;
                    break;
                }
            }
            if (!matched)
            {
                Bucket bucket;
                bucket.signature = signature;
                bucket.exts.push_back(ext.ext);
                bucket.sampleExt = ext.ext;
                for (const CensusItem& item : ext.items)
                {
                    if (item.brand == brand || item.separator) bucket.items.push_back(item);
                }
                // 只保留这一家的条目（分隔线按原样保留在序列里也行，这里简单过滤掉）
                buckets.push_back(std::move(bucket));
            }
        }
    }

    for (auto& entry : brands)
    {
        const std::wstring& brand = entry.first;
        for (Bucket& bucket : entry.second)
        {
            CensusCluster cluster;
            cluster.brand = brand;
            cluster.exts = bucket.exts;
            cluster.items = bucket.items;
            cluster.sampleExt = bucket.sampleExt;

            // 自动命名：品牌 + 覆盖的扩展名（前 3 个）
            std::wstring label = brand;
            if (!cluster.exts.empty())
            {
                label += L" · ";
                for (size_t i = 0; i < cluster.exts.size() && i < 3; ++i)
                {
                    if (i > 0) label += L" ";
                    label += cluster.exts[i];
                }
                if (cluster.exts.size() > 3)
                {
                    label += L" 等 " + std::to_wstring(cluster.exts.size()) + L" 种";
                }
            }
            cluster.label = label;
            report.clusters.push_back(std::move(cluster));
        }
    }

    // 排序：品牌按重要度（系统最后），簇按扩展名数量降序
    auto brandRank = [](const std::wstring& brand)
    {
        if (brand == L"WPS") return 0;
        if (brand == L"7-Zip") return 1;
        if (brand == L"WinRAR") return 2;
        if (brand == L"TortoiseSVN") return 3;
        if (brand == L"ima") return 4;
        if (brand == L"Tailscale") return 5;
        if (brand == L"Git") return 6;
        if (brand == L"Visual Studio") return 7;
        if (brand == L"ChatGPT") return 8;
        if (brand == L"批量工具") return 9;
        if (brand == L"系统") return 100;
        return 10;
    };

    std::sort(report.clusters.begin(), report.clusters.end(),
              [&brandRank](const CensusCluster& a, const CensusCluster& b)
              {
                  const int ra = brandRank(a.brand);
                  const int rb = brandRank(b.brand);
                  if (ra != rb) return ra < rb;
                  if (a.brand != b.brand) return a.brand < b.brand;
                  return a.exts.size() > b.exts.size();
              });
}

// ---------------------------------------------------------------------------
// 合并菜单 + 按「出现范围完全相同」分组
// ---------------------------------------------------------------------------
namespace
{

std::wstring LowerText(const std::wstring& text)
{
    std::wstring lowered = text;
    for (wchar_t& ch : lowered)
    {
        ch = static_cast<wchar_t>(::towlower(ch));
    }
    return lowered;
}

// 一份菜单快照（扩展名菜单或场景菜单）
struct MenuSnapshot
{
    const std::vector<CensusItem>* items = nullptr;
    std::wstring ext;             // 空 = 场景菜单（不记扩展名）
};

// 把多份菜单快照合并成一片森林：
//   · 合并键 = 同级同父 + 文字 + 命令（命令忽略大小写），不区分软件品牌；
//   · 每个节点的 exts = 它出现过的扩展名并集；
//   · 用「索引 + 父索引」建树，避免 vector 扩容让指针失效。
std::vector<CensusSystemNode> MergeMenuSnapshots(const std::vector<MenuSnapshot>& sources)
{
    struct Flat
    {
        std::wstring text;
        std::wstring verb;
        std::wstring iconFile;
        std::vector<std::wstring> exts;
        int parent = -1;
    };

    std::vector<Flat> flat;
    std::map<std::wstring, int> byKey;   // 合并键 → Flat 下标
    std::vector<int> stack;              // 层级 → 当前节点下标

    for (const MenuSnapshot& source : sources)
    {
        if (source.items == nullptr) continue;
        stack.clear();

        for (const CensusItem& item : *source.items)
        {
            if (item.separator) continue;

            const int level = item.level;
            int parent = -1;
            if (level > 0 && static_cast<int>(stack.size()) >= level)
            {
                parent = stack[level - 1];
            }

            std::wstring key = std::to_wstring(parent);
            key.push_back(L'\x1f');
            key += LowerText(item.text);
            key.push_back(L'\x1f');
            key += LowerText(item.verb);

            int index = -1;
            auto found = byKey.find(key);
            if (found != byKey.end())
            {
                index = found->second;
                Flat& existing = flat[index];
                if (existing.iconFile.empty() && !item.iconFile.empty())
                {
                    existing.iconFile = item.iconFile;
                }
                if (!source.ext.empty())
                {
                    bool known = false;
                    for (const std::wstring& knownExt : existing.exts)
                    {
                        if (knownExt == source.ext) { known = true; break; }
                    }
                    if (!known) existing.exts.push_back(source.ext);
                }
            }
            else
            {
                Flat fresh;
                fresh.text     = item.text;
                fresh.verb     = item.verb;
                fresh.iconFile = item.iconFile;
                fresh.parent   = parent;
                if (!source.ext.empty()) fresh.exts.push_back(source.ext);
                flat.push_back(std::move(fresh));
                index = static_cast<int>(flat.size()) - 1;
                byKey[key] = index;
            }

            stack.resize(static_cast<size_t>(level) + 1, -1);
            stack[level] = index;
        }
    }

    // 装回树（childrenByParent 按创建顺序，菜单顺序保持原样）
    std::vector<std::vector<int>> childrenByParent(flat.size());
    std::vector<int> roots;
    for (int i = 0; i < static_cast<int>(flat.size()); ++i)
    {
        if (flat[i].parent >= 0) childrenByParent[flat[i].parent].push_back(i);
        else roots.push_back(i);
    }

    std::function<CensusSystemNode(int)> buildNode = [&](int index) -> CensusSystemNode
    {
        CensusSystemNode node;
        node.text     = flat[index].text;
        node.verb     = flat[index].verb;
        node.iconFile = flat[index].iconFile;
        node.exts     = flat[index].exts;
        for (int child : childrenByParent[index])
        {
            node.children.push_back(buildNode(child));
        }
        return node;
    };

    std::vector<CensusSystemNode> result;
    for (int root : roots)
    {
        result.push_back(buildNode(root));
    }
    return result;
}

// 扩展名集合 → 稳定的分组键（排序后拼接）
std::wstring ExtSetKey(std::vector<std::wstring>& exts)
{
    std::sort(exts.begin(), exts.end());
    std::wstring key;
    for (const std::wstring& ext : exts)
    {
        key += ext;
        key.push_back(L'\x1f');
    }
    return key;
}

} // namespace

void BuildMenuGroups(CensusReport& report)
{
    report.fileGroups.clear();

    // 代表样本优先用最常见的类型，保证菜单顺序看起来眼熟；
    // 其余扩展名按字母序补在后面（只影响先后顺序，不影响合并结果）。
    static const wchar_t* const kPreferred[] = { L".txt", L".docx", L".jpg", L".png", L".zip", L".pdf" };

    std::vector<const CensusExtension*> ordered;
    for (const wchar_t* ext : kPreferred)
    {
        for (const CensusExtension& candidate : report.extensions)
        {
            if (candidate.ok && candidate.ext == ext) { ordered.push_back(&candidate); break; }
        }
    }
    for (const CensusExtension& candidate : report.extensions)
    {
        if (!candidate.ok) continue;
        bool already = false;
        for (const CensusExtension* existing : ordered)
        {
            if (existing->ext == candidate.ext) { already = true; break; }
        }
        if (!already) ordered.push_back(&candidate);
    }

    std::vector<MenuSnapshot> sources;
    sources.reserve(ordered.size());
    for (const CensusExtension* ext : ordered)
    {
        sources.push_back({ &ext->items, ext->ext });
    }

    std::vector<CensusSystemNode> roots = MergeMenuSnapshots(sources);

    // 顶层项按「覆盖的扩展名完全相同」归组：这就是界面上的「一类」
    std::map<std::wstring, size_t> groupIndex;
    for (CensusSystemNode& root : roots)
    {
        std::vector<std::wstring> exts = root.exts;
        const std::wstring key = ExtSetKey(exts);

        auto found = groupIndex.find(key);
        size_t index = 0;
        if (found == groupIndex.end())
        {
            CensusMenuGroup group;
            group.exts = exts;
            report.fileGroups.push_back(std::move(group));
            index = report.fileGroups.size() - 1;
            groupIndex[key] = index;
        }
        else
        {
            index = found->second;
        }
        report.fileGroups[index].items.push_back(std::move(root));
    }

    // 排序：覆盖扩展名多的组在前（"全部通用"自然排第一），同样多的按扩展名排序
    std::sort(report.fileGroups.begin(), report.fileGroups.end(),
              [](const CensusMenuGroup& a, const CensusMenuGroup& b)
              {
                  if (a.exts.size() != b.exts.size()) return a.exts.size() > b.exts.size();
                  return a.exts < b.exts;
              });

    // 三个非文件场景各自合并成一份菜单（不分组、不按软件拆）
    for (CensusScene& scene : report.scenes)
    {
        std::vector<MenuSnapshot> one = { { &scene.items, std::wstring() } };
        scene.mergedMenu = MergeMenuSnapshots(one);
    }
}

// -------------------------------------------------------------------------------------------------------------------------------------------
// JSON 读写
// -------------------------------------------------------------------------------------------------------------------------------------------
namespace
{

json ItemToJson(const CensusItem& item)
{
    json j;
    j["level"]     = item.level;
    j["sep"]       = item.separator;
    j["disabled"]  = item.disabled;
    j["text"]      = ToUtf8(item.text);
    j["verb"]      = ToUtf8(item.verb);
    j["brand"]     = ToUtf8(item.brand);
    j["pathKey"]   = ToUtf8(item.pathKey);
    j["icon"]      = ToUtf8(item.iconFile);
    return j;
}

CensusItem ItemFromJson(const json& j)
{
    CensusItem item;
    item.level    = j.value("level", 0);
    item.separator = j.value("sep", false);
    item.disabled = j.value("disabled", false);
    item.text     = FromUtf8(j.value("text", std::string()));
    item.verb     = FromUtf8(j.value("verb", std::string()));
    item.brand    = FromUtf8(j.value("brand", std::string()));
    item.pathKey  = FromUtf8(j.value("pathKey", std::string()));
    item.iconFile = FromUtf8(j.value("icon", std::string()));
    return item;
}

json ItemsToJson(const std::vector<CensusItem>& items)
{
    json array = json::array();
    for (const CensusItem& item : items) array.push_back(ItemToJson(item));
    return array;
}

std::vector<CensusItem> ItemsFromJson(const json& array)
{
    std::vector<CensusItem> items;
    if (array.is_array())
    {
        for (const auto& j : array) items.push_back(ItemFromJson(j));
    }
    return items;
}

} // namespace

const int kCensusVersion = 2;

bool LoadCensus(CensusReport& report)
{
    report = CensusReport();

    const std::wstring path = CensusFilePath();
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    json root;
    try { in >> root; }
    catch (const std::exception& e)
    {
        Logger::Warn(std::wstring(L"普查缓存解析失败：") + FromUtf8(e.what()));
        return false;
    }

    if (root.value("version", 0) != kCensusVersion) return false;

    report.scannedAt = FromUtf8(root.value("scannedAt", std::string()));

    if (root.contains("extensions") && root["extensions"].is_array())
    {
        for (const auto& j : root["extensions"])
        {
            CensusExtension ext;
            ext.ext        = FromUtf8(j.value("ext", std::string()));
            ext.ok         = j.value("ok", false);
            ext.failReason = FromUtf8(j.value("fail", std::string()));
            ext.items      = ItemsFromJson(j.value("items", json::array()));
            report.extensions.push_back(std::move(ext));
        }
    }

    if (root.contains("scenes") && root["scenes"].is_array())
    {
        for (const auto& j : root["scenes"])
        {
            CensusScene scene;
            scene.scene      = j.value("scene", 0u);
            scene.ok         = j.value("ok", false);
            scene.failReason = FromUtf8(j.value("fail", std::string()));
            scene.items      = ItemsFromJson(j.value("items", json::array()));
            report.scenes.push_back(std::move(scene));
        }
    }

    BuildClusters(report);
    BuildMenuGroups(report);
    return true;
}

bool SaveCensus(const CensusReport& report)
{
    const std::wstring path = CensusFilePath();
    if (path.empty()) return false;

    json root;
    root["version"]   = kCensusVersion;
    root["scannedAt"] = ToUtf8(report.scannedAt);

    json exts = json::array();
    for (const CensusExtension& ext : report.extensions)
    {
        json j;
        j["ext"]   = ToUtf8(ext.ext);
        j["ok"]    = ext.ok;
        j["fail"]  = ToUtf8(ext.failReason);
        j["items"] = ItemsToJson(ext.items);
        exts.push_back(j);
    }
    root["extensions"] = exts;

    json scenes = json::array();
    for (const CensusScene& scene : report.scenes)
    {
        json j;
        j["scene"] = scene.scene;
        j["ok"]    = scene.ok;
        j["fail"]  = ToUtf8(scene.failReason);
        j["items"] = ItemsToJson(scene.items);
        scenes.push_back(j);
    }
    root["scenes"] = scenes;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const std::string text = root.dump(2);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return true;
}

// -------------------------------------------------------------------------------------------------------------------------------------------
// 注册表：扩展名枚举与印记
// -------------------------------------------------------------------------------------------------------------------------------------------
namespace
{

std::vector<std::wstring> EnumSubKeys(HKEY root, const std::wstring& path)
{
    std::vector<std::wstring> names;
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return names;

    wchar_t buffer[256] = {};
    for (DWORD index = 0;; ++index)
    {
        DWORD length = ARRAYSIZE(buffer);
        if (::RegEnumKeyExW(key, index, buffer, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
        {
            break;
        }
        names.emplace_back(buffer);
    }
    ::RegCloseKey(key);
    return names;
}

std::wstring ReadStringValue(HKEY root, const std::wstring& path, const wchar_t* name)
{
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return std::wstring();

    wchar_t buffer[512] = {};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    DWORD type = 0;
    std::wstring value;
    if (::RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS
        && (type == REG_SZ || type == REG_EXPAND_SZ))
    {
        value.assign(buffer, size / sizeof(wchar_t));
        while (!value.empty() && value.back() == L'\0') value.pop_back();
    }
    ::RegCloseKey(key);
    return value;
}

} // namespace

std::vector<std::wstring> EnumerateExtensions()
{
    std::vector<std::wstring> result;
    const std::vector<std::wstring> names = EnumSubKeys(HKEY_CLASSES_ROOT, L"");
    for (const std::wstring& name : names)
    {
        if (name.size() > 1 && name[0] == L'.')
        {
            result.push_back(name);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::wstring RegistryStamp()
{
    // 扩展名数量 + 全局 shellex 键的修改时间 + 常见扩展名键的修改时间
    std::wstring stamp;
    const std::vector<std::wstring> exts = EnumerateExtensions();
    stamp = std::to_wstring(exts.size());
    stamp += L"|";
    stamp += exts.empty() ? L"" : exts.front();
    stamp += L"|";
    stamp += exts.empty() ? L"" : exts.back();

    const wchar_t* watchKeys[] = {
        L"*\\shellex\\ContextMenuHandlers",
        L"AllFilesystemObjects\\shellex\\ContextMenuHandlers",
        L"Directory\\shellex\\ContextMenuHandlers",
        L"Directory\\Background\\shellex\\ContextMenuHandlers",
        L"Folder\\shellex\\ContextMenuHandlers",
    };

    for (const wchar_t* watch : watchKeys)
    {
        HKEY key = nullptr;
        if (::RegOpenKeyExW(HKEY_CLASSES_ROOT, watch, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        FILETIME ft = {};
        if (::RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                               nullptr, nullptr, nullptr, nullptr, &ft) == ERROR_SUCCESS)
        {
            stamp += L"|";
            stamp += std::to_wstring(ft.dwLowDateTime) + L":" + std::to_wstring(ft.dwHighDateTime);
        }
        ::RegCloseKey(key);
    }

    return stamp;
}

// -------------------------------------------------------------------------------------------------------------------------------------------
// 品牌 → 程序/DLL 路径（供"一键补图标"用）
// -------------------------------------------------------------------------------------------------------------------------------------------
namespace
{

std::wstring ResolveHandlerDll(const std::wstring& handlerName)
{
    // 处理器名可能是 CLSID，也可能直接用 DLL 路径
    std::wstring clsid = handlerName;
    if (clsid.size() >= 2 && clsid.front() == L'{' )
    {
        const std::wstring inproc = std::wstring(L"CLSID\\") + clsid + L"\\InprocServer32";
        const std::wstring dll = ReadStringValue(HKEY_CLASSES_ROOT, inproc, nullptr);
        if (!dll.empty()) return dll;
    }
    else if (::PathFileExistsW(handlerName.c_str()))
    {
        return handlerName;
    }
    return std::wstring();
}

std::wstring BrandKeyword(const std::wstring& brand)
{
    if (brand == L"WPS") return L"wps";
    if (brand == L"7-Zip") return L"7-zip";
    if (brand == L"WinRAR") return L"winrar";
    if (brand == L"TortoiseSVN") return L"tortoise";
    if (brand == L"ima") return L"ima";
    if (brand == L"Tailscale") return L"tailscale";
    if (brand == L"Git") return L"git";
    if (brand == L"Visual Studio") return L"visual";
    if (brand == L"ChatGPT") return L"chatgpt";
    if (brand == L"批量工具") return L"batchprint";
    return std::wstring();
}

} // namespace

std::vector<std::wstring> BrandExecutablePaths(const std::wstring& brand)
{
    std::vector<std::wstring> paths;
    const std::wstring keyword = BrandKeyword(brand);
    if (keyword.empty()) return paths;

    const wchar_t* roots[] = {
        L"*\\shellex\\ContextMenuHandlers",
        L"AllFilesystemObjects\\shellex\\ContextMenuHandlers",
        L"Directory\\shellex\\ContextMenuHandlers",
        L"Directory\\Background\\shellex\\ContextMenuHandlers",
        L"Folder\\shellex\\ContextMenuHandlers",
    };

    for (const wchar_t* root : roots)
    {
        for (const std::wstring& handler : EnumSubKeys(HKEY_CLASSES_ROOT, root))
        {
            std::wstring lowerHandler = handler;
            std::transform(lowerHandler.begin(), lowerHandler.end(), lowerHandler.begin(), ::towlower);
            if (lowerHandler.find(keyword) == std::wstring::npos)
            {
                // 名字不含关键字时，试着解析成 DLL 再按文件名匹配
                const std::wstring dll = ResolveHandlerDll(handler);
                if (dll.empty()) continue;
                std::wstring lowerDll = dll;
                std::transform(lowerDll.begin(), lowerDll.end(), lowerDll.begin(), ::towlower);
                if (lowerDll.find(keyword) == std::wstring::npos) continue;
                if (std::find(paths.begin(), paths.end(), dll) == paths.end()) paths.push_back(dll);
                continue;
            }

            const std::wstring dll = ResolveHandlerDll(handler);
            if (!dll.empty() && std::find(paths.begin(), paths.end(), dll) == paths.end())
            {
                paths.push_back(dll);
            }
        }
    }
    return paths;
}

namespace
{

bool SaveHIconAsBmp(HICON icon, const std::wstring& path)
{
    if (icon == nullptr) return false;

    const int size = 32;
    BITMAPINFO dibInfo = {};
    dibInfo.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    dibInfo.bmiHeader.biWidth       = size;
    dibInfo.bmiHeader.biHeight      = size;
    dibInfo.bmiHeader.biPlanes      = 1;
    dibInfo.bmiHeader.biBitCount    = 32;
    dibInfo.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = ::GetDC(nullptr);
    HBITMAP canvas = ::CreateDIBSection(screen, &dibInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    if (!canvas || !bits) return false;

    HDC memDc = ::CreateCompatibleDC(nullptr);
    HGDIOBJ old = ::SelectObject(memDc, canvas);
    RECT full = { 0, 0, size, size };
    ::FillRect(memDc, &full, static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH)));
    ::DrawIconEx(memDc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
    ::SelectObject(memDc, old);
    ::DeleteDC(memDc);

    const int stride = size * 4;
    const DWORD imageSize = static_cast<DWORD>(stride) * size;

    BITMAPFILEHEADER fileHeader = {};
    fileHeader.bfType    = 0x4D42;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize    = fileHeader.bfOffBits + imageSize;

    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = false;
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        ok = ::WriteFile(file, &fileHeader, sizeof(fileHeader), &written, nullptr)
             && ::WriteFile(file, &dibInfo.bmiHeader, sizeof(BITMAPINFOHEADER), &written, nullptr)
             && ::WriteFile(file, bits, imageSize, &written, nullptr);
        ::CloseHandle(file);
    }

    ::DeleteObject(canvas);
    return ok;
}

} // namespace

std::wstring BrandIconFile(const std::wstring& brand)
{
    const std::wstring dir = IconDir();
    if (dir.empty()) return std::wstring();

    const std::wstring name = L"b" + HashKey(brand) + L".bmp";
    const std::wstring path = dir + L"\\" + name;
    if (!::PathFileExistsW(path.c_str())) return std::wstring();
    return name;
}

std::wstring ExtractBrandIcon(const std::wstring& brand)
{
    std::vector<std::wstring> candidates = BrandExecutablePaths(brand);
    if (candidates.empty()) return std::wstring();

    const std::wstring dir = IconDir();
    if (dir.empty()) return std::wstring();

    const std::wstring name = L"b" + HashKey(brand) + L".bmp";
    const std::wstring path = dir + L"\\" + name;
    if (::PathFileExistsW(path.c_str())) return name;

    for (const std::wstring& candidate : candidates)
    {
        HICON largeIcon = nullptr;
        HICON smallIcon = nullptr;
        UINT count = ::ExtractIconExW(candidate.c_str(), 0, &largeIcon, &smallIcon, 1);
        if (count == 0) continue;

        const bool ok = SaveHIconAsBmp(largeIcon ? largeIcon : smallIcon, path);
        if (largeIcon) ::DestroyIcon(largeIcon);
        if (smallIcon) ::DestroyIcon(smallIcon);
        if (ok) return name;
    }
    return std::wstring();
}

// -------------------------------------------------------------------------------------------------------------------------------------------
// 子进程：批量采集
// -------------------------------------------------------------------------------------------------------------------------------------------
namespace
{

std::wstring BatchResultPath(int batchIndex)
{
    const std::wstring appData = AppDataDir();
    if (appData.empty()) return std::wstring();
    wchar_t name[64] = {};
    ::swprintf_s(name, L"menu-census-batch-%d.json", batchIndex);
    return appData + L"\\FileExplorer\\" + name;
}

} // namespace

int RunCensusWorker(int argc, wchar_t** argv)
{
    // argv[2]=批次号  argv[3]=扩展名清单文件  argv[4]=结果文件
    if (argc < 5) return 2;

    const int batchIndex = _wtoi(argv[2]);
    const std::wstring listFile = argv[3];
    const std::wstring outFile = argv[4];

    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::vector<std::wstring> extensions;
    {
        std::ifstream in(listFile, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        std::string content = buffer.str();
        if (content.size() >= 3 && content[0] == '\xEF' && content[1] == '\xBB' && content[2] == '\xBF')
        {
            content.erase(0, 3);
        }
        std::istringstream lines(content);
        std::string line;
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            extensions.push_back(FromUtf8(line));
        }
    }

    const std::wstring dir = SampleDir();

    json result;
    result["batch"] = batchIndex;
    json exts = json::array();

    for (const std::wstring& ext : extensions)
    {
        CensusExtension collected;
        CollectExtension(dir, ext, collected);

        json j;
        j["ext"]   = ToUtf8(collected.ext);
        j["ok"]    = collected.ok;
        j["fail"]  = ToUtf8(collected.failReason);
        j["items"] = ItemsToJson(collected.items);
        exts.push_back(j);
    }
    result["extensions"] = exts;

    std::ofstream out(outFile, std::ios::binary | std::ios::trunc);
    if (out)
    {
        const std::string text = result.dump();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (SUCCEEDED(hr)) ::CoUninitialize();
    return 0;
}

bool RunCensusInChildProcesses(const std::wstring& exePath,
                               const std::vector<std::wstring>& extensions,
                               std::vector<CensusExtension>& outExtensions,
                               const std::function<void(int, int)>& progress)
{
    outExtensions.clear();
    if (extensions.empty()) return false;

    const int batchCount = static_cast<int>((extensions.size() + kBatchSize - 1) / kBatchSize);
    bool anySuccess = false;

    for (int batch = 0; batch < batchCount; ++batch)
    {
        const int begin = batch * kBatchSize;
        const int end   = std::min(static_cast<int>(extensions.size()), begin + kBatchSize);

        // 写清单文件
        const std::wstring listFile = BatchResultPath(batch) + L".list";
        {
            std::ofstream out(listFile, std::ios::binary | std::ios::trunc);
            for (int i = begin; i < end; ++i)
            {
                const std::string line = ToUtf8(extensions[i]);
                out.write(line.data(), static_cast<std::streamsize>(line.size()));
                out.put('\n');
            }
        }

        const std::wstring resultFile = BatchResultPath(batch);

        std::wstring commandLine = L"\"" + exePath + L"\" --census-worker " +
                                   std::to_wstring(batch) + L" \"" + listFile + L"\" \"" + resultFile + L"\"";
        std::wstring mutableCommand = commandLine;

        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process = {};

        const BOOL created = ::CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                                              CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
        if (!created)
        {
            Logger::Warn(L"普查子进程启动失败，批次 " + std::to_wstring(batch));
            ::DeleteFileW(listFile.c_str());
            if (progress) progress(batch + 1, batchCount);
            continue;
        }

        // 等待：每批最多 120 秒
        ::WaitForSingleObject(process.hProcess, 120000);
        DWORD exitCode = 1;
        ::GetExitCodeProcess(process.hProcess, &exitCode);
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
        ::DeleteFileW(listFile.c_str());

        // 读结果
        std::ifstream in(resultFile, std::ios::binary);
        bool batchOk = false;
        if (in && exitCode == 0)
        {
            json root;
            try { in >> root; } catch (...) { root = json(); }
            if (root.contains("extensions") && root["extensions"].is_array())
            {
                for (const auto& j : root["extensions"])
                {
                    CensusExtension ext;
                    ext.ext        = FromUtf8(j.value("ext", std::string()));
                    ext.ok         = j.value("ok", false);
                    ext.failReason = FromUtf8(j.value("fail", std::string()));
                    ext.items      = ItemsFromJson(j.value("items", json::array()));
                    outExtensions.push_back(std::move(ext));
                    batchOk = batchOk || ext.ok;
                }
            }
        }
        ::DeleteFileW(resultFile.c_str());

        if (!batchOk)
        {
            // 这批崩了：把清单里每个扩展名记成"未验证"
            for (int i = begin; i < end; ++i)
            {
                CensusExtension ext;
                ext.ext = extensions[i];
                ext.ok = false;
                ext.failReason = L"采集进程异常";
                outExtensions.push_back(std::move(ext));
            }
        }
        anySuccess = anySuccess || batchOk;

        if (progress) progress(batch + 1, batchCount);
    }

    if (progress) progress(batchCount, batchCount);
    return anySuccess;
}

} // namespace shell
