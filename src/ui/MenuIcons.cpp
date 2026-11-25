// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/MenuIcons.cpp
//
// 右键菜单小图标缓存。说明见 MenuIcons.h。
// ============================================================================

#include "ui/MenuIcons.h"

#include "core/Logger.h"
#include "shell/IconPaths.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace
{

// 把各种来源的图标统一成 16x16（系统给的可能带点尺寸差异，菜单里必须齐）
HICON NormalizeIcon(HICON source, int size)
{
    if (source == nullptr)
    {
        return nullptr;
    }

    // 注意：变量千万别叫 small —— rpcndr.h 里有 `#define small char` 这个经典宏，
    // 会把 "HICON small" 变成 "HICON char"。
    HICON resized = reinterpret_cast<HICON>(::CopyImage(source, IMAGE_ICON, size, size, 0));
    ::DestroyIcon(source);

    return resized;
}

// ---------------------------------------------------------------------------
// 用 Windows 自带的 WIC 解码 PNG。WPS 的传统菜单只给 PNG 图标资产，
// 个别项（文档比对）没有对应的 ICO，只能走这里读 PNG；不引入第三方库。
// ---------------------------------------------------------------------------
HICON LoadPngIcon(const std::wstring& path, int size)
{
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory))) || !factory)
    {
        return nullptr;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                   WICDecodeMetadataCacheOnLoad, &decoder)) || !decoder)
    {
        return nullptr;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)) || !frame)
    {
        return nullptr;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)))
    {
        return nullptr;
    }
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
    {
        return nullptr;
    }

    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(&scaler)))
    {
        return nullptr;
    }
    if (FAILED(scaler->Initialize(converter.Get(), static_cast<UINT>(size),
                                  static_cast<UINT>(size),
                                  WICBitmapInterpolationModeHighQualityCubic)))
    {
        return nullptr;
    }

    const UINT stride = static_cast<UINT>(size) * 4;
    std::vector<BYTE> pixels(static_cast<size_t>(stride) * static_cast<size_t>(size));
    if (FAILED(scaler->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()),
                                  pixels.data())))
    {
        return nullptr;
    }

    // 32 位 BGRA DIB：CreateIconIndirect 会保留 alpha，菜单上不会再出现黑底。
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = size;   // 正高度是自下而上，CreateIconIndirect 更稳妥
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP color = ::CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (color == nullptr || bits == nullptr)
    {
        if (color != nullptr) ::DeleteObject(color);
        return nullptr;
    }

    // WIC 的 CopyPixels 是自上而下；DIB 用正高度时内存是自下而上，需要逐行翻转。
    BYTE* destination = static_cast<BYTE*>(bits);
    for (int y = 0; y < size; ++y)
    {
        std::memcpy(destination + static_cast<size_t>(size - 1 - y) * stride,
                    pixels.data() + static_cast<size_t>(y) * stride,
                    stride);
    }

    HBITMAP mask = ::CreateBitmap(size, size, 1, 1, nullptr);
    if (mask == nullptr)
    {
        ::DeleteObject(color);
        return nullptr;
    }

    // SetMenuItemBitmaps（带子菜单的项走这条路径）只有 AND mask，不认 32 位 alpha。
    // 必须按 alpha 生成 mask：透明像素置 1（不画），不透明像素置 0（画彩色位图）。
    // 否则 WPS AI 这种“父项”会整块黑底。
    HDC maskDc = ::CreateCompatibleDC(nullptr);
    if (maskDc != nullptr)
    {
        HGDIOBJ oldMask = ::SelectObject(maskDc, mask);
        ::PatBlt(maskDc, 0, 0, size, size, BLACKNESS);
        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
            {
                const BYTE alpha = pixels[static_cast<size_t>(y) * stride +
                                          static_cast<size_t>(x) * 4 + 3];
                if (alpha < 128)
                {
                    ::SetPixelV(maskDc, x, y, RGB(255, 255, 255));
                }
            }
        }
        ::SelectObject(maskDc, oldMask);
        ::DeleteDC(maskDc);
    }

    ICONINFO info = {};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON icon = ::CreateIconIndirect(&info);

    ::DeleteObject(mask);
    ::DeleteObject(color);
    return icon;
}

// 菜单里图标统一尺寸（96 DPI 下 16px，高 DPI 由系统缩放菜单时一起放大）
const int kMenuIconSize = 16;

} // 匿名命名空间

MenuIconCache::~MenuIconCache()
{
    Shutdown();

    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& pair : m_icons)
    {
        if (pair.second != nullptr)
        {
            ::DestroyIcon(pair.second);
        }
    }
    m_icons.clear();
}

std::wstring MenuIconCache::KeyForProgram(const core::OpenWithItem& item)
{
    // 和账本的身份串保持一致：exe 是小写全路径，商店应用是 "uwp:<AUMID>"
    return L"program:" + core::OpenWithKey(item);
}

std::wstring MenuIconCache::KeyForNewItem(const shell::ShellNewItem& item)
{
    // 用注册表键名做键：扩展名（".zip"）和 ProgID（"Folder"）都不会撞
    return L"new:" + item.menuKey;
}

std::wstring MenuIconCache::KeyForFile(const std::wstring& iconFile)
{
    std::wstring key = iconFile;
    std::transform(key.begin(), key.end(), key.begin(), ::towlower);
    return L"file:" + key;
}

std::wstring MenuIconCache::KeyForOwner(const std::wstring& server, const std::wstring& appName)
{
    std::wstring key = server + L"|" + appName;
    std::transform(key.begin(), key.end(), key.begin(), ::towlower);
    return L"owner:" + key;
}

std::wstring MenuIconCache::KeyForPath(const std::wstring& path)
{
    std::wstring key = path;
    std::transform(key.begin(), key.end(), key.begin(), ::towlower);
    return L"path:" + key;
}

// ---------------------------------------------------------------------------
// 解析 "C:\Windows\system32\shell32.dll,-16769" 这种图标引用，取出 16px 图标
// ---------------------------------------------------------------------------
HICON LoadIconFromSpec(const std::wstring& spec)
{
    if (spec.empty())
    {
        return nullptr;
    }

    std::wstring text = spec;

    // 去掉资源字符串前缀和引号
    if (!text.empty() && text.front() == L'@')
    {
        text.erase(text.begin());
    }
    if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"')
    {
        text = text.substr(1, text.size() - 2);
    }

    // 最后一个逗号后面是索引（负数表示资源 ID）
    const size_t comma = text.find_last_of(L',');
    if (comma == std::wstring::npos)
    {
        return nullptr;
    }

    const std::wstring filePart = text.substr(0, comma);
    const int index = _wtoi(text.substr(comma + 1).c_str());

    HICON smallIcon = nullptr;
    if (SUCCEEDED(::SHDefExtractIconW(filePart.c_str(), index, 0, nullptr, &smallIcon,
                                      kMenuIconSize)) &&
        smallIcon != nullptr)
    {
        return NormalizeIcon(smallIcon, kMenuIconSize);
    }

    return nullptr;
}

HICON MenuIconCache::LoadProgramIcon(const core::OpenWithItem& item)
{
    // ---- Win32 程序：直接问它的 exe ----
    if (item.kind != L"uwp")
    {
        if (item.path.empty())
        {
            return nullptr;
        }

        SHFILEINFOW info = {};
        if (::SHGetFileInfoW(item.path.c_str(), 0, &info, sizeof(info),
                             SHGFI_ICON | SHGFI_SMALLICON))
        {
            return NormalizeIcon(info.hIcon, kMenuIconSize);
        }

        return nullptr;
    }

    // ---- 商店应用：先解析成 shell 项，再问图标 ----
    if (item.appId.empty())
    {
        return nullptr;
    }

    const std::wstring parsingName = L"shell:AppsFolder\\" + item.appId;

    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(::SHParseDisplayName(parsingName.c_str(), nullptr, &pidl, 0, nullptr)) ||
        pidl == nullptr)
    {
        return nullptr;
    }

    SHFILEINFOW info = {};
    HICON icon = nullptr;

    if (::SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &info, sizeof(info),
                         SHGFI_ICON | SHGFI_SMALLICON | SHGFI_PIDL))
    {
        icon = NormalizeIcon(info.hIcon, kMenuIconSize);
    }

    ::CoTaskMemFree(pidl);
    return icon;
}

HICON MenuIconCache::LoadNewItemIcon(const shell::ShellNewItem& item)
{
    // ---- 1. ShellNew 的 IconPath（系统菜单用的就是它，最准）----
    if (!item.iconPath.empty())
    {
        HICON fromSpec = LoadIconFromSpec(item.iconPath);
        if (fromSpec != nullptr)
        {
            return fromSpec;
        }
    }

    // ---- 2. 有模板文件就用模板本身的图标（比如 WinRAR 的 zipnew.dat）----
    if (!item.templatePath.empty())
    {
        SHFILEINFOW info = {};
        if (::SHGetFileInfoW(item.templatePath.c_str(), 0, &info, sizeof(info),
                             SHGFI_ICON | SHGFI_SMALLICON))
        {
            return NormalizeIcon(info.hIcon, kMenuIconSize);
        }
    }

    // ---- 3. 文件夹：直接用系统的文件夹图标 ----
    if (item.kind == shell::ShellNewKind::Folder)
    {
        SHSTOCKICONINFO stock = {};
        stock.cbSize = sizeof(stock);

        if (SUCCEEDED(::SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICON | SHGSI_SMALLICON, &stock)) &&
            stock.hIcon != nullptr)
        {
            return NormalizeIcon(stock.hIcon, kMenuIconSize);
        }
    }

    // ---- 4. 按扩展名问系统（假文件名，不碰真实文件）----
    std::wstring fakeName;

    if (!item.extension.empty())
    {
        fakeName = L"x" + item.extension;
    }
    else if (!item.menuKey.empty() && item.menuKey.front() == L'.')
    {
        fakeName = L"x" + item.menuKey;
    }

    if (!fakeName.empty())
    {
        SHFILEINFOW info = {};
        if (::SHGetFileInfoW(fakeName.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                             SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES))
        {
            return NormalizeIcon(info.hIcon, kMenuIconSize);
        }
    }

    // ---- 5. 还是没有：用该 ProgID 的 DefaultIcon ----
    if (!item.menuKey.empty())
    {
        HKEY rowKey = nullptr;
        if (::RegOpenKeyExW(HKEY_CLASSES_ROOT, (item.menuKey + L"\\DefaultIcon").c_str(), 0,
                            KEY_READ | KEY_WOW64_64KEY, &rowKey) == ERROR_SUCCESS)
        {
            std::wstring spec;

            wchar_t buffer[512] = {};
            DWORD   size = sizeof(buffer);
            if (::RegQueryValueExW(rowKey, nullptr, nullptr, nullptr,
                                   reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS &&
                buffer[0] != L'\0')
            {
                spec = buffer;
            }

            ::RegCloseKey(rowKey);

            if (!spec.empty())
            {
                return LoadIconFromSpec(spec);
            }
        }
    }

    return nullptr;
}

namespace
{

std::wstring MenuIconFullPath(const std::wstring& iconFile)
{
    if (iconFile.empty()) return std::wstring();
    if (iconFile.find(L'\\') != std::wstring::npos ||
        iconFile.find(L'/') != std::wstring::npos ||
        iconFile.find(L':') != std::wstring::npos)
    {
        return iconFile;
    }

    const std::wstring dir = shell::IconDir();
    if (dir.empty()) return std::wstring();
    return dir + L"\\" + iconFile;
}

std::wstring NormalizeOwnerAppName(const std::wstring& text)
{
    std::wstring out;
    for (wchar_t ch : text)
    {
        if (::iswalnum(ch)) out.push_back(static_cast<wchar_t>(::towlower(ch)));
    }
    return out;
}

// 菜单图标落盘时背景可能是白色，也可能是黑色（owner-draw 扩展把透明像素
// 画成了黑色）。加载时先看四个角，按占多数的角颜色做透明键，避免整个黑块
// 被当成图标背景画到菜单上。
COLORREF MenuIconMaskColor(HBITMAP bitmap)
{
    if (bitmap == nullptr) return RGB(255, 255, 255);

    HDC dc = ::CreateCompatibleDC(nullptr);
    if (dc == nullptr) return RGB(255, 255, 255);

    HGDIOBJ old = ::SelectObject(dc, bitmap);
    const COLORREF corners[4] = {
        ::GetPixel(dc, 0, 0),
        ::GetPixel(dc, 15, 0),
        ::GetPixel(dc, 0, 15),
        ::GetPixel(dc, 15, 15)
    };
    ::SelectObject(dc, old);
    ::DeleteDC(dc);

    // 统计出现最多的角颜色；CLR_INVALID 忽略。
    COLORREF best = RGB(255, 255, 255);
    int bestCount = 0;
    for (COLORREF color : corners)
    {
        if (color == CLR_INVALID) continue;
        int count = 0;
        for (COLORREF other : corners)
        {
            if (other == color) ++count;
        }
        if (count > bestCount)
        {
            best = color;
            bestCount = count;
        }
    }
    return best;
}

} // namespace

HICON MenuIconCache::LoadFileIcon(const std::wstring& iconFile)
{
    const std::wstring path = MenuIconFullPath(iconFile);
    if (path.empty()) return nullptr;

    // 新采集的动态菜单图标是 32 位 alpha ICO，直接按图标加载，保留透明通道。
    // 旧缓存里的 .bmp 仍按下面的兼容路径处理。
    const size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos && _wcsicmp(path.c_str() + dot, L".ico") == 0)
    {
        HICON icon = static_cast<HICON>(::LoadImageW(
            nullptr, path.c_str(), IMAGE_ICON, 16, 16, LR_LOADFROMFILE));
        if (icon != nullptr) return NormalizeIcon(icon, kMenuIconSize);
    }

    // WPS 传统菜单资产是 PNG；用 WIC 解成带 alpha 的 HICON。
    if (dot != std::wstring::npos && _wcsicmp(path.c_str() + dot, L".png") == 0)
    {
        HICON icon = LoadPngIcon(path, kMenuIconSize);
        if (icon != nullptr) return NormalizeIcon(icon, kMenuIconSize);
    }

    HBITMAP bitmap = static_cast<HBITMAP>(::LoadImageW(
        nullptr, path.c_str(), IMAGE_BITMAP, 16, 16,
        LR_LOADFROMFILE | LR_CREATEDIBSECTION));
    if (bitmap == nullptr) return nullptr;

    HIMAGELIST list = ::ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 1, 1);
    if (list == nullptr)
    {
        ::DeleteObject(bitmap);
        return nullptr;
    }

    const int index = ::ImageList_AddMasked(list, bitmap, MenuIconMaskColor(bitmap));
    ::DeleteObject(bitmap);
    if (index < 0)
    {
        ::ImageList_Destroy(list);
        return nullptr;
    }

    HICON icon = ::ImageList_GetIcon(list, index, ILD_TRANSPARENT);
    ::ImageList_Destroy(list);
    return icon != nullptr ? NormalizeIcon(icon, kMenuIconSize) : nullptr;
}

HICON MenuIconCache::LoadOwnerIcon(const std::wstring& server, const std::wstring& appName)
{
    if (server.empty()) return nullptr;

    HICON largeIcon = nullptr;
    HICON smallIcon = nullptr;
    const UINT count = ::ExtractIconExW(server.c_str(), 0, &largeIcon, &smallIcon, 1);
    if (largeIcon != nullptr) ::DestroyIcon(largeIcon);
    if (smallIcon != nullptr) return NormalizeIcon(smallIcon, kMenuIconSize);
    if (count > 0) return nullptr;

    // DLL 本身没有图标时，到同目录找和应用名同名的 EXE（WPS → wpsoffice.exe）。
    const size_t slash = server.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash + 1 >= server.size()) return nullptr;

    const std::wstring dir = server.substr(0, slash);
    const std::wstring wanted = NormalizeOwnerAppName(appName);
    if (wanted.empty()) return nullptr;

    WIN32_FIND_DATAW data = {};
    const std::wstring pattern = dir + L"\\*.exe";
    HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return nullptr;

    HICON result = nullptr;
    do
    {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        std::wstring name = data.cFileName;
        const size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos) name.erase(dot);
        if (NormalizeOwnerAppName(name) != wanted) continue;

        const std::wstring exe = dir + L"\\" + data.cFileName;
        HICON exeLarge = nullptr;
        HICON exeSmall = nullptr;
        const UINT exeCount = ::ExtractIconExW(exe.c_str(), 0, &exeLarge, &exeSmall, 1);
        if (exeLarge != nullptr) ::DestroyIcon(exeLarge);
        if (exeCount > 0 && exeSmall != nullptr)
        {
            result = NormalizeIcon(exeSmall, kMenuIconSize);
            break;
        }
    } while (::FindNextFileW(find, &data));
    ::FindClose(find);
    return result;
}

void MenuIconCache::Store(const std::wstring& key, HICON icon)
{
    if (key.empty() || icon == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_icons.find(key) != m_icons.end())
    {
        // 已经有了（并发重复取），这份多余的直接丢掉
        ::DestroyIcon(icon);
        return;
    }

    m_icons.emplace(key, icon);
}

HICON MenuIconCache::Get(const std::wstring& key)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_icons.find(key);
    return (it != m_icons.end()) ? it->second : nullptr;
}

HICON MenuIconCache::LoadPathIcon(const std::wstring& path)
{
    if (path.empty())
    {
        return nullptr;
    }

    SHFILEINFOW info = {};
    if (::SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info),
                         SHGFI_ICON | SHGFI_SMALLICON))
    {
        return NormalizeIcon(info.hIcon, kMenuIconSize);
    }
    return nullptr;
}

HICON MenuIconCache::GetFile(const std::wstring& iconFile)
{
    if (iconFile.empty()) return nullptr;

    const std::wstring key = KeyForFile(iconFile);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_icons.find(key);
        if (it != m_icons.end()) return it->second;
    }

    HICON icon = LoadFileIcon(iconFile);
    if (icon == nullptr) return nullptr;

    Store(key, icon);

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_icons.find(key);
    return (it != m_icons.end()) ? it->second : nullptr;
}

HICON MenuIconCache::GetOwnerIcon(const std::wstring& server, const std::wstring& appName)
{
    if (server.empty()) return nullptr;

    const std::wstring key = KeyForOwner(server, appName);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_icons.find(key);
        if (it != m_icons.end()) return it->second;
    }

    HICON icon = LoadOwnerIcon(server, appName);
    if (icon == nullptr) return nullptr;

    Store(key, icon);

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_icons.find(key);
    return (it != m_icons.end()) ? it->second : nullptr;
}

void MenuIconCache::PrefetchPrograms(const std::vector<core::OpenWithItem>& programs)
{
    if (programs.empty())
    {
        return;
    }

    // 先看有没有缺的，全都有就不必开线程
    bool needWork = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const core::OpenWithItem& item : programs)
        {
            if (m_icons.find(KeyForProgram(item)) == m_icons.end())
            {
                needWork = true;
                break;
            }
        }
    }

    if (!needWork)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pendingPrograms.insert(m_pendingPrograms.end(), programs.begin(), programs.end());
    }

    StartWorker();
}

void MenuIconCache::PrefetchNewItems(const std::vector<shell::ShellNewItem>& items)
{
    if (items.empty())
    {
        return;
    }

    bool needWork = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const shell::ShellNewItem& item : items)
        {
            if (m_icons.find(KeyForNewItem(item)) == m_icons.end())
            {
                needWork = true;
                break;
            }
        }
    }

    if (!needWork)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pendingNewItems.insert(m_pendingNewItems.end(), items.begin(), items.end());
    }

    StartWorker();
}

void MenuIconCache::PrefetchPaths(const std::vector<std::wstring>& paths)
{
    if (paths.empty())
    {
        return;
    }

    bool needWork = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const std::wstring& path : paths)
        {
            if (path.empty() || path.find(L'%') != std::wstring::npos) continue;

            const DWORD attr = ::GetFileAttributesW(path.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                continue;
            }

            if (m_icons.find(KeyForPath(path)) == m_icons.end())
            {
                needWork = true;
                break;
            }
        }
    }

    if (!needWork)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pendingPaths.insert(m_pendingPaths.end(), paths.begin(), paths.end());
    }

    StartWorker();
}

void MenuIconCache::StartWorker()
{
    if (m_workerRunning)
    {
        return;   // 已经有一个在跑了，它会把新加进来的活一起干完
    }

    m_workerRunning = true;

    // 上一个线程对象已经结束的话先回收
    if (m_worker.joinable())
    {
        m_worker.join();
    }

    m_worker = std::thread([this]()
    {
        // 后台线程也要初始化 COM：SHParseDisplayName / SHGetFileInfo 都要用
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        while (true)
        {
            std::vector<core::OpenWithItem>   programs;
            std::vector<shell::ShellNewItem>  newItems;
            std::vector<std::wstring>         paths;

            {
                std::lock_guard<std::mutex> lock(m_mutex);

                if (m_pendingPrograms.empty() && m_pendingNewItems.empty() &&
                    m_pendingPaths.empty())
                {
                    m_workerRunning = false;
                    break;
                }

                programs.swap(m_pendingPrograms);
                newItems.swap(m_pendingNewItems);
                paths.swap(m_pendingPaths);
            }

            for (const core::OpenWithItem& item : programs)
            {
                Store(KeyForProgram(item), LoadProgramIcon(item));
            }

            for (const shell::ShellNewItem& item : newItems)
            {
                Store(KeyForNewItem(item), LoadNewItemIcon(item));
            }

            for (const std::wstring& path : paths)
            {
                Store(KeyForPath(path), LoadPathIcon(path));
            }
        }

        ::CoUninitialize();
    });
}

void MenuIconCache::Shutdown()
{
    if (m_worker.joinable())
    {
        m_worker.join();
    }

    m_workerRunning = false;
}
