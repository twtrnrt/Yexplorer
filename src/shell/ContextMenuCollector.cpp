// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ContextMenuCollector.cpp
//
// 按场景采集系统右键菜单快照 + 每条菜单项渲染成位图 + 缓存读写。
//
// 采集（和资源管理器右键菜单完全同一套 COM）：
//   · 文件 / 文件夹：IShellItem::BindToHandler(BHID_SFUIObject) → IContextMenu
//   · 空白区：目录的 IShellFolder::GetUIObjectOf(0 个子项) → IContextMenu
//   · 多选：父 IShellFolder::GetUIObjectOf(2 个子 PIDL) → IContextMenu
//
// 渲染（解决图标问题）：
//   实测 434 项里只有 29% 自带 hbmpItem，WPS / 7-Zip 全空。
//   真实菜单是扩展自己在 WM_DRAWITEM 里画的，所以这里对每条项：
//     1. 建 32 位内存 DC（白底）；
//     2. 调 IContextMenu3::HandleMenuMsg2(WM_DRAWITEM)，让扩展把图标+文字画上去；
//     3. 扩展不画（返回失败 / 全白）时退回：画 hbmpItem（若有）+ 自己画文字；
//     4. 存成 BMP，界面按原样贴出来。
//
// 注意：本模块只在「配置右键菜单」窗口打开 / 点刷新时调用，不进右键弹出链路。
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

#include <algorithm>
#include <cwctype>
#include <fstream>

#pragma comment(lib, "shlwapi.lib")

namespace shell
{

namespace
{

// 渲染尺寸（96 DPI 逻辑像素；显示时按窗口 DPI 缩放）
const int kRowWidth  = 460;
const int kRowHeight = 26;

// 渲染统计（每次采集前清零；用于判断有多少行真的是扩展自己画的）
int g_nativeDrawCount   = 0;   // 扩展自己画的
int g_fallbackDrawCount = 0;   // 扩展没画、由我们兜底画的

// 当前正在采集第几份场景快照（渲染图文件名用它保证唯一）
int g_sceneSerial = 0;

// ---------------------------------------------------------------------------
// 基础小工具
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

std::wstring AppDataFolder()
{
    LPWSTR appData = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) ||
        appData == nullptr)
    {
        return std::wstring();
    }
    std::wstring path = appData;
    ::CoTaskMemFree(appData);
    return path;
}

std::wstring CacheFilePath()
{
    const std::wstring appData = AppDataFolder();
    if (appData.empty())
    {
        return std::wstring();
    }
    return appData + L"\\FileExplorer\\menu-cache.json";
}

bool EnsureDirectory(const std::wstring& filePath)
{
    const size_t slash = filePath.find_last_of(L'\\');
    if (slash == std::wstring::npos)
    {
        return false;
    }
    int result = ::SHCreateDirectoryExW(nullptr, filePath.substr(0, slash).c_str(), nullptr);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS;
}

std::wstring NowTimeString()
{
    SYSTEMTIME st = {};
    ::GetLocalTime(&st);
    wchar_t buffer[32] = {};
    ::swprintf_s(buffer, L"%04d-%02d-%02d %02d:%02d:%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

// 去掉助记符 '&'（"&&" 保留一个 &）
std::wstring StripAmpersand(std::wstring text)
{
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == L'&')
        {
            if (i + 1 < text.size() && text[i + 1] == L'&')
            {
                out.push_back(L'&');
                ++i;
                continue;
            }
            continue;   // 单个 & 是助记符，去掉
        }
        out.push_back(text[i]);
    }
    return out;
}

// 清空并重建渲染图目录（每次采集都重来，避免旧图残留）
void ResetIconDir(const std::wstring& dir)
{
    if (dir.empty())
    {
        return;
    }
    ::SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

    WIN32_FIND_DATAW data = {};
    HANDLE find = ::FindFirstFileW((dir + L"\\*.bmp").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                ::DeleteFileW((dir + L"\\" + data.cFileName).c_str());
            }
        } while (::FindNextFileW(find, &data));
        ::FindClose(find);
    }
}

// ---------------------------------------------------------------------------
// 样本
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

void CleanupSample(const std::wstring& root)
{
    if (root.empty())
    {
        return;
    }

    WIN32_FIND_DATAW data = {};
    HANDLE find = ::FindFirstFileW((root + L"\\*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..")
            {
                continue;
            }
            const std::wstring child = root + L"\\" + name;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                CleanupSample(child);
            }
            else
            {
                ::SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                ::DeleteFileW(child.c_str());
            }
        } while (::FindNextFileW(find, &data));
        ::FindClose(find);
    }
    ::RemoveDirectoryW(root.c_str());
}

bool PrepareSample(std::wstring& rootOut, std::wstring& folderOut)
{
    rootOut = SampleRootPath();
    if (rootOut.empty())
    {
        return false;
    }

    CleanupSample(rootOut);
    ::SHCreateDirectoryExW(nullptr, rootOut.c_str(), nullptr);

    folderOut = rootOut + L"\\sample-folder";
    ::SHCreateDirectoryExW(nullptr, folderOut.c_str(), nullptr);

    const wchar_t* files[] =
    {
        L"sample.txt", L"sample.docx", L"sample.rar", L"sample.zip",
        L"multi-a.txt", L"multi-b.txt",
    };
    for (const wchar_t* file : files)
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
// 渲染一条菜单项到 BMP
//
// 返回 true 表示图片已写盘；失败时 item.imageFile 留空，界面只画文字。
// ---------------------------------------------------------------------------
bool SaveDibAsBmp(const std::wstring& path, const BITMAPINFO& info, const void* bits)
{
    const int width  = info.bmiHeader.biWidth;
    const int height = info.bmiHeader.biHeight;      // 正数 = 自下而上，和 BMP 一致
    if (width <= 0 || height <= 0 || bits == nullptr)
    {
        return false;
    }

    const int bytesPerPixel = info.bmiHeader.biBitCount / 8;
    const int stride = width * bytesPerPixel;
    const DWORD imageSize = static_cast<DWORD>(stride) * static_cast<DWORD>(height);

    BITMAPFILEHEADER fileHeader = {};
    fileHeader.bfType    = 0x4D42;   // "BM"
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize    = fileHeader.bfOffBits + imageSize;

    BITMAPINFOHEADER header = info.bmiHeader;

    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    bool ok = true;
    DWORD written = 0;
    ok = ok && ::WriteFile(file, &fileHeader, sizeof(fileHeader), &written, nullptr);
    ok = ok && ::WriteFile(file, &header, sizeof(header), &written, nullptr);
    ok = ok && ::WriteFile(file, bits, imageSize, &written, nullptr);
    ::CloseHandle(file);

    return ok;
}

// 整张图是否全白（判断扩展到底有没有画东西）
bool LooksBlank(const void* bits, int width, int height)
{
    const unsigned char* pixels = static_cast<const unsigned char*>(bits);
    const int stride = width * 4;
    for (int y = 0; y < height; ++y)
    {
        const unsigned char* row = pixels + y * stride;
        for (int x = 0; x < width * 4; x += 4)
        {
            if (row[x] != 0xFF || row[x + 1] != 0xFF || row[x + 2] != 0xFF)
            {
                return false;
            }
        }
    }
    return true;
}

// 菜单字体（和系统右键菜单同一号字）
HFONT MenuFont()
{
    static HFONT font = nullptr;
    static bool tried = false;

    if (!tried)
    {
        tried = true;
        NONCLIENTMETRICSW metrics = {};
        metrics.cbSize = sizeof(metrics);
        if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
        {
            font = ::CreateFontIndirectW(&metrics.lfMenuFont);
        }
        if (font == nullptr)
        {
            font = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
        }
    }
    return font;
}

bool RenderRow(HWND host,
               IContextMenu3* cm3,
               const MENUITEMINFOW& mi,
               UINT_PTR itemOffset,
               const std::wstring& text,
               bool disabled,
               const std::wstring& outPath)
{
    // 先问扩展希望多宽多高（多数不填，用默认）
    int width  = kRowWidth;
    int height = kRowHeight;

    if (cm3 != nullptr)
    {
        MEASUREITEMSTRUCT measure = {};
        measure.CtlType  = ODT_MENU;
        measure.itemID   = static_cast<UINT>(itemOffset);
        measure.itemData = mi.dwItemData;
        if (SUCCEEDED(cm3->HandleMenuMsg2(WM_MEASUREITEM, 0,
                                          reinterpret_cast<LPARAM>(&measure), nullptr)))
        {
            if (measure.itemWidth > 0 && measure.itemWidth < 700)
            {
                width = static_cast<int>(measure.itemWidth);
            }
            if (measure.itemHeight > 0 && measure.itemHeight <= 48)
            {
                height = static_cast<int>(measure.itemHeight);
            }
        }
    }

    // 32 位 DIB（自下而上，和 BMP 存储顺序一致，可直接落盘）
    BITMAPINFO info = {};
    info.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth       = width;
    info.bmiHeader.biHeight      = height;
    info.bmiHeader.biPlanes      = 1;
    info.bmiHeader.biBitCount    = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = ::GetDC(nullptr);
    HBITMAP bitmap = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, screen);

    if (bitmap == nullptr || bits == nullptr)
    {
        return false;
    }

    HDC memDc = ::CreateCompatibleDC(nullptr);
    HGDIOBJ oldBitmap = ::SelectObject(memDc, bitmap);
    HGDIOBJ oldFont   = ::SelectObject(memDc, MenuFont());

    // 白底（和系统菜单背景一致）
    RECT full = { 0, 0, width, height };
    ::FillRect(memDc, &full, static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH)));
    ::SetBkMode(memDc, TRANSPARENT);

    bool drawn = false;
    if (cm3 != nullptr)
    {
        DRAWITEMSTRUCT draw = {};
        draw.CtlType    = ODT_MENU;
        draw.CtlID      = 0;
        draw.itemID     = static_cast<UINT>(itemOffset);
        draw.itemAction = ODA_DRAWENTIRE;
        draw.itemState  = disabled ? ODS_GRAYED : 0;
        draw.hwndItem   = host;
        draw.hDC        = memDc;
        draw.rcItem     = full;
        draw.itemData   = mi.dwItemData;

        HRESULT hr = cm3->HandleMenuMsg2(WM_DRAWITEM, 0,
                                         reinterpret_cast<LPARAM>(&draw), nullptr);
        drawn = SUCCEEDED(hr) && !LooksBlank(bits, width, height);

        // 有些老扩展只实现了 IContextMenu2::HandleMenuMsg（不认 HandleMenuMsg2），
        // 比如 7-Zip：它的项是 owner-draw，文字要靠它自己画才正确。
        if (!drawn)
        {
            wil::com_ptr<IContextMenu2> cm2;
            if (SUCCEEDED(cm3->QueryInterface(IID_PPV_ARGS(cm2.put()))) && cm2 != nullptr)
            {
                // 重画一遍：先清回白底，免得留下半张图
                ::FillRect(memDc, &full, static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH)));
                cm2->HandleMenuMsg(WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&draw));
                drawn = !LooksBlank(bits, width, height);
            }
        }
    }

    // 扩展没画（或者没实现 WM_DRAWITEM）：自己兜底画图标 + 文字
    if (!drawn)
    {
        ++g_fallbackDrawCount;

        // 图标列固定留 22 像素宽，保证所有行的文字左边界对齐
        const int iconLeft = 4;

        // 有些项在 QueryContextMenu 时就设了图标位图，这里用上
        if (mi.hbmpItem != nullptr && mi.hbmpItem != HBMMENU_CALLBACK)
        {
            BITMAP bmpInfo = {};
            if (::GetObjectW(mi.hbmpItem, sizeof(bmpInfo), &bmpInfo) == sizeof(bmpInfo))
            {
                const int iconSize = 16;
                HDC iconDc = ::CreateCompatibleDC(memDc);
                HGDIOBJ oldIcon = ::SelectObject(iconDc, mi.hbmpItem);
                ::SetStretchBltMode(memDc, HALFTONE);
                ::StretchBlt(memDc, iconLeft, (height - iconSize) / 2, iconSize, iconSize,
                             iconDc, 0, 0, bmpInfo.bmWidth, bmpInfo.bmHeight, SRCCOPY);
                ::SelectObject(iconDc, oldIcon);
                ::DeleteDC(iconDc);
            }
        }

        ::SetTextColor(memDc, disabled ? RGB(109, 109, 109) : RGB(0, 0, 0));
        RECT textRect = { iconLeft + 22, 0, width - 6, height };
        ::DrawTextW(memDc, text.c_str(), -1, &textRect,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    else
    {
        ++g_nativeDrawCount;
    }

    ::SelectObject(memDc, oldFont);
    ::SelectObject(memDc, oldBitmap);
    ::DeleteDC(memDc);

    const bool saved = SaveDibAsBmp(outPath, info, bits);
    ::DeleteObject(bitmap);

    return saved;
}

// ---------------------------------------------------------------------------
// 递归遍历菜单，生成菜单项快照
//
// itemOffset = 命令 ID - idFirst（GetCommandString / 渲染都用这个）
// -------------------------------------------------------------------------------------------------------------------------------------------
void WalkMenu(HMENU menu,
              IContextMenu3* cm3,
              HWND host,
              UINT idFirst,
              int level,
              unsigned int scene,
              const std::wstring& iconDir,
              int& itemCounter,
              bool& anyFail,
              std::vector<ContextMenuItem>& out)
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

        ContextMenuItem item;
        item.level  = level;
        item.scenes = scene;

        // 分隔线：保留位置（界面画一条浅线），层级也照原样
        if ((mi.fType & MFT_SEPARATOR) != 0)
        {
            item.isSeparator = true;
            out.push_back(item);
            continue;
        }

        wchar_t textBuffer[512] = {};
        ::GetMenuStringW(menu, index, textBuffer, ARRAYSIZE(textBuffer), MF_BYPOSITION);
        item.text     = StripAmpersand(textBuffer);
        item.disabled = (mi.fState & MFS_GRAYED) != 0;
        item.fromExtension = true;

        const UINT offset = (mi.wID != 0 && mi.wID != static_cast<UINT>(-1))
                                ? mi.wID - idFirst : static_cast<UINT>(-1);
        if (offset != static_cast<UINT>(-1))
        {
            item.commandOffset = static_cast<int>(offset);

            wchar_t verbBuffer[256] = {};
            HRESULT verbHr = cm3->GetCommandString(offset, GCS_VERBW, nullptr,
                                                   reinterpret_cast<LPSTR>(verbBuffer),
                                                   ARRAYSIZE(verbBuffer));
            if (SUCCEEDED(verbHr) && verbBuffer[0] != L'\0')
            {
                item.verb = verbBuffer;
            }
        }

        // 渲染这一项（图标 + 文字由扩展自己画）
        // 文件名用「快照序号_项序号」：四个文件样本是四次独立快照，序号必须唯一，
        // 否则同名文件会互相覆盖（曾经四个文件样本都写成 s1_N.bmp，图全串了）。
        const std::wstring imageName =
            L"m" + std::to_wstring(g_sceneSerial) + L"_" + std::to_wstring(itemCounter) + L".bmp";
        ++itemCounter;

        const std::wstring imagePath = iconDir + L"\\" + imageName;
        if (offset != static_cast<UINT>(-1))
        {
            if (RenderRow(host, cm3, mi, offset, item.text, item.disabled, imagePath))
            {
                item.imageFile = imageName;
            }
            else
            {
                item.failed = true;
                item.failReason = L"渲染失败";
                anyFail = true;
            }
        }
        else
        {
            // 子菜单节点 / 无命令 ID 的项：文字照常展示
            item.imageFile.clear();
        }

        out.push_back(item);

        // 子菜单：先让扩展填充（WM_INITMENUPOPUP），再递归
        if (mi.hSubMenu != nullptr)
        {
            LPARAM initParam = MAKELPARAM(index, FALSE);
            HRESULT initHr = E_NOTIMPL;
            if (cm3 != nullptr)
            {
                initHr = cm3->HandleMenuMsg2(WM_INITMENUPOPUP,
                                             reinterpret_cast<WPARAM>(mi.hSubMenu),
                                             initParam, nullptr);
            }

            // 老实现只认 IContextMenu2::HandleMenuMsg 的，退回一次
            if (FAILED(initHr))
            {
                wil::com_ptr<IContextMenu2> cm2;
                if (SUCCEEDED(cm3->QueryInterface(IID_PPV_ARGS(cm2.put()))) && cm2 != nullptr)
                {
                    cm2->HandleMenuMsg(WM_INITMENUPOPUP,
                                       reinterpret_cast<WPARAM>(mi.hSubMenu), initParam);
                }
            }

            WalkMenu(mi.hSubMenu, cm3, host, idFirst, level + 1, scene,
                     iconDir, itemCounter, anyFail, out);
        }
    }
}

// ---------------------------------------------------------------------------
// 对单个 IContextMenu 采一份场景快照
// ----------------------------------------------------------------------------------// ---------------------------------------------------------------------------
void CollectOneScene(IContextMenu* cm,
                     IContextMenu3* cm3,
                     HWND host,
                     unsigned int scene,
                     const std::wstring& sampleLabel,
                     const std::wstring& samplePath,
                     const std::wstring& iconDir,
                     SceneMenu& outScene)
{
    outScene.scene       = scene;
    outScene.sampleLabel = sampleLabel;
    outScene.samplePath  = samplePath;

    ++g_sceneSerial;   // 每份快照一个唯一序号

    if (cm == nullptr || cm3 == nullptr)
    {
        outScene.ok = false;
        outScene.failReason = L"拿不到 IContextMenu3";
        return;
    }

    const UINT idFirst = 1;
    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr)
    {
        outScene.ok = false;
        outScene.failReason = L"创建菜单失败";
        return;
    }

    HRESULT hr = cm->QueryContextMenu(menu, 0, idFirst, 0x7FFF, CMF_NORMAL);
    if (SUCCEEDED(hr))
    {
        int counter = 0;
        bool anyFail = false;
        WalkMenu(menu, cm3, host, idFirst, 0, scene, iconDir,
                 counter, anyFail, outScene.items);
        outScene.ok = true;
        if (anyFail)
        {
            outScene.failReason = L"部分条目渲染失败";
        }
    }
    else
    {
        outScene.ok = false;
        outScene.failReason = L"QueryContextMenu 失败：" + Logger::FormatHresult(hr);
    }

    ::DestroyMenu(menu);
}

// 文件 / 文件夹样本
void CollectForPath(const std::wstring& path,
                    unsigned int scene,
                    const std::wstring& label,
                    HWND host,
                    const std::wstring& iconDir,
                    ContextMenuReport& report)
{
    SceneMenu sceneMenu;
    sceneMenu.scene       = scene;
    sceneMenu.sampleLabel = label;
    sceneMenu.samplePath  = path;

    wil::com_ptr<IShellItem> item;
    if (FAILED(::SHCreateItemFromParsingName(path.c_str(), nullptr,
                                              IID_PPV_ARGS(item.put()))) ||
        item == nullptr)
    {
        sceneMenu.failReason = L"解析路径失败";
        report.scenes.push_back(std::move(sceneMenu));
        return;
    }

    wil::com_ptr<IContextMenu> cm;
    if (FAILED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(cm.put()))) ||
        cm == nullptr)
    {
        sceneMenu.failReason = L"拿菜单对象失败";
        report.scenes.push_back(std::move(sceneMenu));
        return;
    }

    wil::com_ptr<IContextMenu3> cm3;
    if (FAILED(cm->QueryInterface(IID_PPV_ARGS(cm3.put()))) || cm3 == nullptr)
    {
        sceneMenu.failReason = L"拿不到 IContextMenu3（无法渲染图标）";
        report.scenes.push_back(std::move(sceneMenu));
        return;
    }

    CollectOneScene(cm.get(), cm3.get(), host, scene, label, path, iconDir, sceneMenu);
    report.scenes.push_back(std::move(sceneMenu));
}

// 空白区样本
void CollectForBackground(const std::wstring& dir,
                          HWND host,
                          const std::wstring& iconDir,
                          ContextMenuReport& report)
{
    SceneMenu sceneMenu;
    sceneMenu.scene       = SceneBackground;
    sceneMenu.sampleLabel = L"文件夹空白区";
    sceneMenu.samplePath  = dir;

    wil::com_ptr<IShellItem> item;
    if (FAILED(::SHCreateItemFromParsingName(dir.c_str(), nullptr,
                                              IID_PPV_ARGS(item.put()))) ||
        item == nullptr)
    {
        sceneMenu.failReason = L"解析目录失败";
        report.scenes.push_back(std::move(sceneMenu));
        return;
    }

    wil::com_ptr<IShellFolder> folder;
    if (FAILED(item->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(folder.put()))) ||
        folder == nullptr)
    {
        sceneMenu.failReason = L"拿 IShellFolder 失败";
        report.scenes.push_back(std::move(sceneMenu));
        return;
    }

    // 空白区菜单有两个来源，按可靠性顺序都试：
    //   1. 文件夹视图的"背景"对象（资源管理器就是从这里拿的背景菜单）；
    //   2. 目录自己的 GetUIObjectOf(0 个子项)——部分命名空间扩展只支持这种方式。
    wil::com_ptr<IContextMenu> cm;

    wil::com_ptr<IShellView> view;
    if (SUCCEEDED(folder->CreateViewObject(host, IID_PPV_ARGS(view.put()))) &&
        view != nullptr)
    {
        view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(cm.put()));
    }

    if (cm == nullptr)
    {
        folder->GetUIObjectOf(nullptr, 0, nullptr, IID_IContextMenu,
                              nullptr, cm.put_void());
    }

    if (cm == nullptr)
    {
        sceneMenu.failReason = L"拿空白区菜单失败（两种方式都没拿到）";
        report.scenes.push_back(std::move(sceneMenu));
        return;
    }

    wil::com_ptr<IContextMenu3> cm3;
    if (FAILED(cm->QueryInterface(IID_PPV_ARGS(cm3.put()))) || cm3 == nullptr)
    {
        sceneMenu.failReason = L"拿不到 IContextMenu3";
        report.scenes.push_back(std::move(sceneMenu));
        return;
    }

    CollectOneScene(cm.get(), cm3.get(), host, SceneBackground,
                    L"文件夹空白区", dir, iconDir, sceneMenu);
    report.scenes.push_back(std::move(sceneMenu));
}

// 多选样本
void CollectForMultiSelect(const std::wstring& path1,
                           const std::wstring& path2,
                           HWND host,
                           const std::wstring& iconDir,
                           ContextMenuReport& report)
{
    SceneMenu sceneMenu;
    sceneMenu.scene       = SceneMultiple;
    sceneMenu.sampleLabel = L"多选两个 txt 文件";
    sceneMenu.samplePath  = path1;

    PIDLIST_ABSOLUTE pidl1 = nullptr;
    PIDLIST_ABSOLUTE pidl2 = nullptr;
    SFGAOF attrs = 0;

    auto finish = [&]()
    {
        if (pidl1 != nullptr)
        {
            ::CoTaskMemFree(pidl1);
        }
        if (pidl2 != nullptr)
        {
            ::CoTaskMemFree(pidl2);
        }
        report.scenes.push_back(std::move(sceneMenu));
    };

    if (FAILED(::SHParseDisplayName(path1.c_str(), nullptr, &pidl1, 0, &attrs)) ||
        pidl1 == nullptr ||
        FAILED(::SHParseDisplayName(path2.c_str(), nullptr, &pidl2, 0, &attrs)) ||
        pidl2 == nullptr)
    {
        sceneMenu.failReason = L"解析样本路径失败";
        finish();
        return;
    }

    wil::com_ptr<IShellFolder> folder;
    PCUITEMID_CHILD child1 = nullptr;
    if (FAILED(::SHBindToParent(pidl1, IID_IShellFolder, folder.put_void(), &child1)) ||
        folder == nullptr || child1 == nullptr)
    {
        sceneMenu.failReason = L"拆父文件夹失败";
        finish();
        return;
    }

    wil::com_ptr<IShellFolder> folder2;
    PCUITEMID_CHILD child2 = nullptr;
    if (FAILED(::SHBindToParent(pidl2, IID_IShellFolder, folder2.put_void(), &child2)) ||
        child2 == nullptr)
    {
        sceneMenu.failReason = L"拆第二个子项失败";
        finish();
        return;
    }

    PCUITEMID_CHILD children[2] = { child1, child2 };

    wil::com_ptr<IContextMenu> cm;
    if (FAILED(folder->GetUIObjectOf(nullptr, 2, children, IID_IContextMenu,
                                     nullptr, cm.put_void())) || cm == nullptr)
    {
        sceneMenu.failReason = L"拿多选菜单失败";
        finish();
        return;
    }

    wil::com_ptr<IContextMenu3> cm3;
    if (FAILED(cm->QueryInterface(IID_PPV_ARGS(cm3.put()))) || cm3 == nullptr)
    {
        sceneMenu.failReason = L"拿不到 IContextMenu3";
        finish();
        return;
    }

    CollectOneScene(cm.get(), cm3.get(), host, SceneMultiple,
                    sceneMenu.sampleLabel, path1, iconDir, sceneMenu);
    finish();
}

// 隐藏宿主窗口：HandleMenuMsg2 的 DRAWITEM 需要 hwndItem
HWND CreateHiddenHost()
{
    const wchar_t* const kHostClass = L"FileExplorerMenuProbeHost";

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = ::DefWindowProcW;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kHostClass;
    ::RegisterClassExW(&wc);

    return ::CreateWindowExW(0, kHostClass, L"", WS_POPUP,
                             0, 0, 1, 1, nullptr, nullptr,
                             ::GetModuleHandleW(nullptr), nullptr);
}

} // namespace

// ---------------------------------------------------------------------------

std::wstring IconDirPath()
{
    const std::wstring appData = AppDataFolder();
    if (appData.empty())
    {
        return std::wstring();
    }
    return appData + L"\\FileExplorer\\menu-icons";
}

void CollectScenes(ContextMenuReport& report)
{
    HRESULT initHr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needUninitialize = SUCCEEDED(initHr);

    report.scenes.clear();
    g_nativeDrawCount   = 0;
    g_fallbackDrawCount = 0;
    g_sceneSerial       = 0;

    std::wstring root, folder;
    if (!PrepareSample(root, folder))
    {
        report.collectedAt = NowTimeString();
        for (const auto& scene : { SceneFile, SceneFolder, SceneBackground, SceneMultiple })
        {
            SceneMenu sceneMenu;
            sceneMenu.scene = scene;
            sceneMenu.failReason = L"样本目录准备失败";
            report.scenes.push_back(std::move(sceneMenu));
        }
        if (needUninitialize) ::CoUninitialize();
        return;
    }

    const std::wstring iconDir = IconDirPath();
    ResetIconDir(iconDir);

    HWND host = CreateHiddenHost();

    try
    {
        // 文件：四个样本各采一份（不同扩展名菜单本来就不同）
        struct FileSample { const wchar_t* name; const wchar_t* label; };
        const FileSample files[] = {
            { L"sample.txt",  L"文件 · sample.txt" },
            { L"sample.docx", L"文件 · sample.docx" },
            { L"sample.rar",  L"文件 · sample.rar" },
            { L"sample.zip",  L"文件 · sample.zip" },
        };
        for (const FileSample& sample : files)
        {
            CollectForPath(root + L"\\" + sample.name, SceneFile, sample.label,
                           host, iconDir, report);
        }

        CollectForPath(folder, SceneFolder, L"文件夹", host, iconDir, report);
        CollectForBackground(root, host, iconDir, report);
        CollectForMultiSelect(root + L"\\multi-a.txt", root + L"\\multi-b.txt",
                              host, iconDir, report);
    }
    catch (...)
    {
        Logger::Error(L"右键菜单采集出现未知异常");
    }

    if (host != nullptr)
    {
        ::DestroyWindow(host);
    }

    CleanupSample(root);

    report.collectedAt = NowTimeString();

    int sceneOk = 0;
    for (const SceneMenu& scene : report.scenes)
    {
        sceneOk += scene.ok ? 1 : 0;
    }
    Logger::Info(L"右键菜单采集完成：场景 " + std::to_wstring(report.scenes.size()) +
                 L"，成功 " + std::to_wstring(sceneOk) +
                 L"，扩展自绘 " + std::to_wstring(g_nativeDrawCount) +
                 L" 行，兜底绘制 " + std::to_wstring(g_fallbackDrawCount) + L" 行");
}

// ---------------------------------------------------------------------------
// 缓存
// ---------------------------------------------------------------------------
using nlohmann::json;

const int kCacheVersion = 3;

ContextMenuItem ItemFromJson(const json& j)
{
    ContextMenuItem item;
    auto str = [&j](const char* key) -> std::wstring
    {
        return j.contains(key) && j[key].is_string()
                   ? FromUtf8(j[key].get<std::string>())
                   : std::wstring();
    };
    item.level         = j.contains("level") ? j["level"].get<int>() : 0;
    item.text          = str("text");
    item.verb          = str("verb");
    item.commandLine   = str("commandLine");
    item.dllPath       = str("dllPath");
    item.clsid         = str("clsid");
    item.scenes        = j.contains("scenes") ? j["scenes"].get<unsigned int>() : 0;
    item.commandOffset = j.contains("commandOffset") ? j["commandOffset"].get<int>() : -1;
    item.isSeparator   = j.contains("isSeparator") && j["isSeparator"].get<bool>();
    item.disabled      = j.contains("disabled") && j["disabled"].get<bool>();
    item.failed        = j.contains("failed") && j["failed"].get<bool>();
    item.fromRegistry  = j.contains("fromRegistry") && j["fromRegistry"].get<bool>();
    item.fromExtension = j.contains("fromExtension") && j["fromExtension"].get<bool>();
    item.imageFile     = str("imageFile");
    item.failReason    = str("failReason");
    return item;
}

json ItemToJson(const ContextMenuItem& item)
{
    json j;
    j["level"]         = item.level;
    j["text"]          = ToUtf8(item.text);
    j["verb"]          = ToUtf8(item.verb);
    j["commandLine"]   = ToUtf8(item.commandLine);
    j["dllPath"]       = ToUtf8(item.dllPath);
    j["clsid"]         = ToUtf8(item.clsid);
    j["scenes"]        = item.scenes;
    j["commandOffset"] = item.commandOffset;
    j["isSeparator"]   = item.isSeparator;
    j["disabled"]      = item.disabled;
    j["failed"]        = item.failed;
    j["fromRegistry"]  = item.fromRegistry;
    j["fromExtension"] = item.fromExtension;
    j["imageFile"]     = ToUtf8(item.imageFile);
    j["failReason"]    = ToUtf8(item.failReason);
    return j;
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

    json scenes = json::array();
    for (const SceneMenu& scene : report.scenes)
    {
        json sj;
        sj["scene"]       = scene.scene;
        sj["sampleLabel"] = ToUtf8(scene.sampleLabel);
        sj["samplePath"]  = ToUtf8(scene.samplePath);
        sj["ok"]          = scene.ok;
        sj["failReason"]  = ToUtf8(scene.failReason);

        json items = json::array();
        for (const ContextMenuItem& item : scene.items)
        {
            items.push_back(ItemToJson(item));
        }
        sj["items"] = items;
        scenes.push_back(sj);
    }
    root["scenes"] = scenes;

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
        Logger::Warn(std::wstring(L"菜单缓存 JSON 解析失败：") + FromUtf8(e.what()));
        return false;
    }

    const int version = root.contains("version") && root["version"].is_number()
                            ? root["version"].get<int>() : 0;
    if (version != kCacheVersion)
    {
        Logger::Info(L"菜单缓存版本不匹配，作废重采");
        return false;
    }

    report.scenes.clear();
    report.collectedAt = root.contains("collectedAt") && root["collectedAt"].is_string()
                             ? FromUtf8(root["collectedAt"].get<std::string>())
                             : std::wstring();

    if (root.contains("scenes") && root["scenes"].is_array())
    {
        for (const auto& sj : root["scenes"])
        {
            SceneMenu scene;
            scene.scene       = sj.contains("scene") ? sj["scene"].get<unsigned int>() : 0;
            scene.sampleLabel = sj.contains("sampleLabel") && sj["sampleLabel"].is_string()
                                    ? FromUtf8(sj["sampleLabel"].get<std::string>()) : std::wstring();
            scene.samplePath  = sj.contains("samplePath") && sj["samplePath"].is_string()
                                    ? FromUtf8(sj["samplePath"].get<std::string>()) : std::wstring();
            scene.ok          = sj.contains("ok") && sj["ok"].get<bool>();
            scene.failReason  = sj.contains("failReason") && sj["failReason"].is_string()
                                    ? FromUtf8(sj["failReason"].get<std::string>()) : std::wstring();

            if (sj.contains("items") && sj["items"].is_array())
            {
                for (const auto& ij : sj["items"])
                {
                    scene.items.push_back(ItemFromJson(ij));
                }
            }
            report.scenes.push_back(std::move(scene));
        }
    }
    return true;
}

} // namespace shell
