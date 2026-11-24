// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ShellIcon.cpp
//
// 图标缓存实现。
// ============================================================================

#include "ShellIcon.h"
#include "core/Logger.h"

#include <cstring>
#include <chrono>
#include <cwctype>
#include <vector>
#include <shlobj.h>      // SHParseDisplayName（虚拟项取图标要靠它换成 PIDL）
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

namespace
{

// ----------------------------------------------------------------------------
// 用「假文件名」按扩展名取系统图标索引
//
// 关键：带 SHGFI_USEFILEATTRIBUTES 标志时，系统不会真的去访问这个文件，
//       只按传入的文件名（扩展名）和属性去图标表里匹配。
//       这是最快的方式，一次几微秒。
// ----------------------------------------------------------------------------
int QueryIconIndexByExtension(const std::wstring& extension, bool large)
{
    // 构造一个假文件名，例如 ".dll" → "file.dll"
    std::wstring fakeName = L"file";
    fakeName += extension;

    SHFILEINFOW fileInfo = {};
    DWORD_PTR result = ::SHGetFileInfoW(
        fakeName.c_str(),
        FILE_ATTRIBUTE_NORMAL,
        &fileInfo,
        sizeof(fileInfo),
        SHGFI_SYSICONINDEX | (large ? SHGFI_LARGEICON : SHGFI_SMALLICON) | SHGFI_USEFILEATTRIBUTES);

    if (result == 0)
    {
        return 0;   // 取不到就用 0 号图标（系统默认文件图标）
    }
    return fileInfo.iIcon;
}

// ----------------------------------------------------------------------------
// 取文件夹图标
//
// 【注意这里踩过坑】
//   一开始用 "C:\\" 当样本，结果系统把它识别成「驱动器」，返回的是硬盘图标，
//   导致列表里所有文件夹都显示成硬盘。
//   正确做法是传一个普通名字（如 "folder"）：配合 SHGFI_USEFILEATTRIBUTES，
//   系统不会去访问它，只按「这是一个目录」来给图标 —— 得到的就是标准黄色文件夹。
// ----------------------------------------------------------------------------
int QueryFolderIconIndex(bool large)
{
    SHFILEINFOW fileInfo = {};
    DWORD_PTR result = ::SHGetFileInfoW(
        L"folder",
        FILE_ATTRIBUTE_DIRECTORY,
        &fileInfo,
        sizeof(fileInfo),
        SHGFI_SYSICONINDEX | (large ? SHGFI_LARGEICON : SHGFI_SMALLICON) | SHGFI_USEFILEATTRIBUTES);

    if (result == 0)
    {
        return 0;
    }
    return fileInfo.iIcon;
}

// ----------------------------------------------------------------------------
// 把 HICON 拆成 32 位 BGRA 像素（落盘用）
//
// 图标里真正决定外观的是颜色位图，透明靠 alpha 通道。
// 有些老图标只有 24 位色（没有 alpha），这时用它的 1 位掩码反推 alpha，
// 免得存下来的图变成"整块不透明"。
// ----------------------------------------------------------------------------
bool ExtractIconBits(HICON icon, std::vector<BYTE>& pixels, int& width, int& height)
{
    if (icon == nullptr)
    {
        return false;
    }

    ICONINFO info = {};
    if (!::GetIconInfo(icon, &info))
    {
        return false;
    }

    const auto cleanup = [&info]()
    {
        if (info.hbmColor != nullptr) { ::DeleteObject(info.hbmColor); }
        if (info.hbmMask  != nullptr) { ::DeleteObject(info.hbmMask);  }
    };

    if (info.hbmColor == nullptr)
    {
        cleanup();
        return false;
    }

    BITMAP bitmap = {};
    if (::GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) == 0)
    {
        cleanup();
        return false;
    }

    width  = bitmap.bmWidth;
    height = bitmap.bmHeight;

    // 图标最大就到 256；超过这个范围的当作异常数据，不存
    if (width <= 0 || height <= 0 || width > 256 || height > 256)
    {
        cleanup();
        return false;
    }

    // 负高度 = 自上而下存，和屏幕显示顺序一致，省得后面又要翻
    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth       = width;
    bitmapInfo.bmiHeader.biHeight      = -height;
    bitmapInfo.bmiHeader.biPlanes      = 1;
    bitmapInfo.bmiHeader.biBitCount    = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);

    HDC screenDc = ::GetDC(nullptr);
    const int lines = ::GetDIBits(screenDc, info.hbmColor, 0, height,
                                  pixels.data(), &bitmapInfo, DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, screenDc);

    if (lines == 0)
    {
        cleanup();
        pixels.clear();
        return false;
    }

    // 先看有没有 alpha
    bool hasAlpha = false;
    for (size_t index = 3; index < pixels.size(); index += 4)
    {
        if (pixels[index] != 0)
        {
            hasAlpha = true;
            break;
        }
    }

    if (!hasAlpha && info.hbmMask != nullptr)
    {
        // 掩码是 1 位：1 = 透明，0 = 不透明
        const int maskStride = ((width + 31) / 32) * 4;
        std::vector<BYTE> mask(static_cast<size_t>(maskStride) * static_cast<size_t>(height), 0);

        BITMAPINFO maskInfo = {};
        maskInfo.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        maskInfo.bmiHeader.biWidth       = width;
        maskInfo.bmiHeader.biHeight      = -height;
        maskInfo.bmiHeader.biPlanes      = 1;
        maskInfo.bmiHeader.biBitCount    = 1;
        maskInfo.bmiHeader.biCompression = BI_RGB;
        maskInfo.bmiColors[0].rgbBlue = 0;
        maskInfo.bmiColors[0].rgbGreen = 0;
        maskInfo.bmiColors[0].rgbRed = 0;
        maskInfo.bmiColors[0].rgbReserved = 0;
        maskInfo.bmiColors[1].rgbBlue = 255;
        maskInfo.bmiColors[1].rgbGreen = 255;
        maskInfo.bmiColors[1].rgbRed = 255;
        maskInfo.bmiColors[1].rgbReserved = 0;

        HDC maskDc = ::GetDC(nullptr);
        ::GetDIBits(maskDc, info.hbmMask, 0, height, mask.data(), &maskInfo, DIB_RGB_COLORS);
        ::ReleaseDC(nullptr, maskDc);

        for (int y = 0; y < height; ++y)
        {
            const BYTE* row = mask.data() + static_cast<size_t>(maskStride) * y;
            for (int x = 0; x < width; ++x)
            {
                const bool transparent = ((row[x / 8] >> (7 - (x % 8))) & 1) != 0;
                pixels[(static_cast<size_t>(y) * width + x) * 4 + 3] = transparent ? 0 : 255;
            }
        }
    }
    else if (!hasAlpha)
    {
        // 连掩码都没有：当作不透明
        for (size_t index = 3; index < pixels.size(); index += 4)
        {
            pixels[index] = 255;
        }
    }

    cleanup();
    return true;
}

// ----------------------------------------------------------------------------
// 把存盘的像素还原成 HICON（透明度靠 alpha，掩码给全 0）
// ----------------------------------------------------------------------------
HICON CreateIconFromBits(int width, int height, const std::vector<BYTE>& pixels)
{
    if (width <= 0 || height <= 0 ||
        pixels.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4u)
    {
        return nullptr;
    }

    BITMAPINFO colorInfo = {};
    colorInfo.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    colorInfo.bmiHeader.biWidth       = width;
    colorInfo.bmiHeader.biHeight      = -height;
    colorInfo.bmiHeader.biPlanes      = 1;
    colorInfo.bmiHeader.biBitCount    = 32;
    colorInfo.bmiHeader.biCompression = BI_RGB;

    void*   colorBits = nullptr;
    HBITMAP colorBitmap = ::CreateDIBSection(nullptr, &colorInfo, DIB_RGB_COLORS,
                                             &colorBits, nullptr, 0);
    if (colorBitmap == nullptr || colorBits == nullptr)
    {
        if (colorBitmap != nullptr) { ::DeleteObject(colorBitmap); }
        return nullptr;
    }
    std::memcpy(colorBits, pixels.data(), pixels.size());

    // 掩码给全 0（不透明）：透明像素靠 alpha = 0 藏起来，ImageList 走 ILC_COLOR32 认 alpha
    const int maskStride = ((width + 15) / 16) * 2;   // CreateBitmap 的 1 位位图按 2 字节对齐
    std::vector<BYTE> maskBits(static_cast<size_t>(maskStride) * static_cast<size_t>(height), 0);
    HBITMAP maskBitmap = ::CreateBitmap(width, height, 1, 1, maskBits.data());

    ICONINFO info = {};
    info.fIcon    = TRUE;
    info.hbmColor = colorBitmap;
    info.hbmMask  = maskBitmap;

    HICON icon = ::CreateIconIndirect(&info);

    ::DeleteObject(colorBitmap);
    if (maskBitmap != nullptr) { ::DeleteObject(maskBitmap); }

    return icon;
}

} // namespace

namespace shell
{

IconCache::IconCache() = default;

IconCache::~IconCache()
{
    // 退出时打一条统计：验证"第二次打开走磁盘、不再读源文件"靠它
    if (m_diskCache != nullptr)
    {
        Logger::Info(L"图标磁盘缓存统计：命中 " + std::to_wstring(m_diskCache->HitCount()) +
                     L" 次，未命中 " + std::to_wstring(m_diskCache->MissCount()) +
                     L" 次，本次新增约 " + std::to_wstring(m_diskCache->CurrentBytes() / 1024) + L" KB");

        // 真去系统挖图标的次数和总耗时：第一次打开目录的"卡"花在哪，看这条
        long long averageMs = (m_extractCount > 0) ? (m_extractTotalMs / m_extractCount) : 0;
        Logger::Info(L"图标提取：去系统挖 " + std::to_wstring(m_extractCount) +
                     L" 次，合计 " + std::to_wstring(m_extractTotalMs) +
                     L" ms（平均 " + std::to_wstring(averageMs) + L" ms/次）");
    }

    // m_systemImageList / m_mediumSystemImageList / m_largeSystemImageList 由 wil::com_ptr 自动释放
    m_hImageList       = nullptr;
    m_hMediumImageList = nullptr;
    m_hLargeImageList  = nullptr;
}

bool IconCache::Initialize()
{
    if (m_initialized)
    {
        return true;
    }

    // SHIL_SMALL     ：系统小图标列表（16px，列表/详细信息视图用这个）
    // SHIL_LARGE     ：系统中图标列表（32px，「打开方式」那两个窗口的列表用）
    // SHIL_EXTRALARGE：系统大图标列表（48px，「大图标」视图用这个）
    // 直接把 IImageList 当 HIMAGELIST 用是官方支持的用法
    HRESULT hr = ::SHGetImageList(SHIL_SMALL, IID_PPV_ARGS(m_systemImageList.put()));
    if (FAILED(hr))
    {
        return false;
    }

    m_hImageList = reinterpret_cast<HIMAGELIST>(m_systemImageList.get());

    // 中图标列表拿不到不算失败：那两个窗口退化成没图标，别的功能不受影响
    if (SUCCEEDED(::SHGetImageList(SHIL_LARGE, IID_PPV_ARGS(m_mediumSystemImageList.put())))
        && m_mediumSystemImageList != nullptr)
    {
        m_hMediumImageList = reinterpret_cast<HIMAGELIST>(m_mediumSystemImageList.get());
    }

    // 大图标列表拿不到不算失败：界面照样能用，只是「大图标」视图会没有图标
    if (SUCCEEDED(::SHGetImageList(SHIL_EXTRALARGE, IID_PPV_ARGS(m_largeSystemImageList.put())))
        && m_largeSystemImageList != nullptr)
    {
        m_hLargeImageList = reinterpret_cast<HIMAGELIST>(m_largeSystemImageList.get());
    }

    // 文件夹图标只查一次，之后所有文件夹共用
    m_folderIconIndex      = QueryFolderIconIndex(false);
    m_largeFolderIconIndex = QueryFolderIconIndex(true);

    // 磁盘成品缓存：exe/lnk 这类"必须读文件"的图标，第一次挖出来后存成小图，
    // 以后直接读小图，不再碰源文件（资源管理器不卡就是靠这一层）
    m_diskCache = std::make_unique<IconDiskCache>();
    if (!m_diskCache->Initialize(L"icons", 32LL * 1024 * 1024))
    {
        m_diskCache.reset();    // 建不出目录就退回"不缓存"，功能不受影响
    }

    m_initialized = true;
    return true;
}

std::wstring IconCache::ExtractExtension(const std::wstring& name)
{
    size_t dotPos = name.find_last_of(L'.');
    if (dotPos == std::wstring::npos || dotPos == 0 || dotPos + 1 >= name.size())
    {
        return L"";
    }

    std::wstring extension = name.substr(dotPos);
    for (auto& ch : extension)
    {
        ch = static_cast<wchar_t>(::towlower(ch));
    }
    return extension;
}

int IconCache::GetIconIndex(const std::wstring& name, bool isFolder, IconSize size)
{
    if (!m_initialized)
    {
        return 0;
    }

    // Small 用 16px 那套；Medium(32) 和 Large(48) 共用大图标那套查询与索引
    const bool large = (size != IconSize::Small);

    // 文件夹：统一图标，零查询
    if (isFolder)
    {
        return large ? m_largeFolderIconIndex : m_folderIconIndex;
    }

    // 文件：按扩展名查缓存（大小图标各一套缓存，不能混用）
    std::wstring extension = ExtractExtension(name);
    auto&        cache     = large ? m_largeExtensionIconCache : m_extensionIconCache;

    auto iterator = cache.find(extension);
    if (iterator != cache.end())
    {
        return iterator->second;
    }

    int iconIndex = QueryIconIndexByExtension(extension, large);
    cache[extension] = iconIndex;
    return iconIndex;
}

int IconCache::GetIconIndexByPath(const std::wstring& path, IconSize size)
{
    if (!m_initialized || path.empty())
    {
        return 0;
    }

    // Small 用 16px 那套；Medium(32) 和 Large(48) 共用大图标那套查询与索引
    const bool large = (size != IconSize::Small);

    SHFILEINFOW fileInfo = {};
    DWORD_PTR result = ::SHGetFileInfoW(
        path.c_str(),
        FILE_ATTRIBUTE_DIRECTORY,
        &fileInfo,
        sizeof(fileInfo),
        SHGFI_SYSICONINDEX | (large ? SHGFI_LARGEICON : SHGFI_SMALLICON));

    if (result == 0)
    {
        return large ? m_largeFolderIconIndex : m_folderIconIndex;
    }
    return fileInfo.iIcon;
}

// ============================================================================
// 按完整路径取图标（exe / lnk 这类"图标长在文件自己身上"的条目用）
//
// 和上面按扩展名取图标的区别只有两点：
//   1. 传给系统的不是假名字，而是真实路径；
//   2. 不带 SHGFI_USEFILEATTRIBUTES，系统会真的去读这个文件（读它内嵌的图标）。
// 结果按路径缓存，同一个文件只问一次。
// ============================================================================

int IconCache::GetIconIndexByFullPath(const std::wstring& path, IconSize size,
                                      unsigned long long fileSize, FILETIME modified)
{
    if (!m_initialized || path.empty())
    {
        return 0;
    }

    // 「C:」这种光秃秃的盘符要补成「C:\」：不补的话系统会把它当成"当前目录"，
    // 取回来的不是驱动器图标
    std::wstring normalized = path;
    if (normalized.size() == 2 && normalized[1] == L':')
    {
        normalized += L'\\';
    }

    // Small 走 16px；Medium(32) / Large(48) 走大图标那套查询。
    // 磁盘缓存只服务 Small / Large：Medium 只有「打开方式」窗口用，量很小，
    // 而且它的 32px 索引和 48px 索引不一定同一套，单独缓存更稳。
    const bool  medium   = (size == IconSize::Medium);
    const bool  large    = (size == IconSize::Large);
    const DWORD sizeFlag = (medium || large) ? SHGFI_LARGEICON : SHGFI_SMALLICON;

    const bool isShellNamespace = (normalized.compare(0, 2, L"::") == 0) ||
                                  (normalized.compare(0, 6, L"shell:") == 0);

    // 只有"真实文件 + 带大小和修改时间"才走磁盘缓存。
    // 这两个值列表枚举时已经有了，直接传进来，省掉一次属性查询；
    // 文件被替换（大小或时间变了）时 key 跟着变，图标自动更新。
    const bool hasIdentity = (modified.dwLowDateTime != 0 || modified.dwHighDateTime != 0);
    const bool useDisk = (m_diskCache != nullptr) && hasIdentity && !medium && !isShellNamespace;

    if (useDisk)
    {
        const std::wstring key = BuildDiskKey(normalized, size, fileSize, modified);

        auto memory = m_diskIndexCache.find(key);
        if (memory != m_diskIndexCache.end())
        {
            return memory->second;      // 本次运行已经取过，连磁盘都不用读
        }

        if (m_diskNegativeKeys.find(key) != m_diskNegativeKeys.end())
        {
            return 0;                   // 确认取不到（例如 exe 里压根没有图标资源）
        }

        const int diskIndex = LoadIconFromDisk(key, size);
        if (diskIndex >= 0)
        {
            m_diskIndexCache[key] = diskIndex;
            return diskIndex;
        }
        if (diskIndex == -2)
        {
            m_diskNegativeKeys.insert(key);
            return 0;
        }

        // 磁盘没有 → 真正去问系统（这一步会读文件，是最贵的）
        const auto      extractStart = std::chrono::steady_clock::now();
        SHFILEINFOW     fileInfo = {};
        DWORD_PTR result = ::SHGetFileInfoW(normalized.c_str(),
                                            0,
                                            &fileInfo,
                                            sizeof(fileInfo),
                                            SHGFI_SYSICONINDEX | SHGFI_ICON | sizeFlag);
        const long long extractMs = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - extractStart).count());

        ++m_extractCount;
        m_extractTotalMs += extractMs;

        int iconIndex = 0;
        if (result != 0 && fileInfo.hIcon != nullptr)
        {
            iconIndex = fileInfo.iIcon;

            // 顺手把图标转成像素存盘：下次（包括重启程序以后）不再读这个文件
            SaveIconToDisk(key, fileInfo.hIcon);
            ::DestroyIcon(fileInfo.hIcon);
        }
        else
        {
            // 确定取不到：记负缓存，避免每次打开目录都重试
            IconDiskCache::Entry negativeEntry;
            negativeEntry.negative = true;
            m_diskCache->Save(IconDiskCache::Kind::Icon, key, negativeEntry);
            m_diskNegativeKeys.insert(key);
        }

        m_diskIndexCache[key] = iconIndex;
        return iconIndex;
    }

    // ---- 旧逻辑（中图标 / 没有大小时间 / Shell 命名空间虚拟项）----
    auto& cache = medium ? m_mediumPathIconCache
                         : (large ? m_largePathIconCache : m_pathIconCache);

    auto cached = cache.find(normalized);
    if (cached != cache.end())
    {
        return cached->second;
    }

    int iconIndex = 0;

    // Shell 命名空间里的虚拟项（「::{GUID}…」「shell:…」这种解析名）
    // 千万不能直接把字符串丢给 SHGetFileInfo：实测它一律返回 0，
    // 结果就是主文件夹 / 控制面板 / 网络 / 回收站全都显示成普通黄色文件夹。
    // 正确做法是先 SHParseDisplayName 换成 PIDL，再用 SHGFI_PIDL 问图标。
    if (isShellNamespace)
    {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(::SHParseDisplayName(normalized.c_str(), nullptr, &pidl, 0, nullptr)) &&
            pidl != nullptr)
        {
            SHFILEINFOW fileInfo = {};
            DWORD_PTR result = ::SHGetFileInfoW(
                reinterpret_cast<LPCWSTR>(pidl),
                0,
                &fileInfo,
                sizeof(fileInfo),
                SHGFI_PIDL | SHGFI_SYSICONINDEX | sizeFlag);

            if (result != 0)
            {
                iconIndex = fileInfo.iIcon;
            }
            ::CoTaskMemFree(pidl);
        }
    }
    else
    {
        SHFILEINFOW fileInfo = {};
        DWORD_PTR result = ::SHGetFileInfoW(
            normalized.c_str(),
            0,                                   // 0：让系统自己去取真实属性
            &fileInfo,
            sizeof(fileInfo),
            SHGFI_SYSICONINDEX | sizeFlag);

        // 取不到就给 0（列表会显示系统默认文件图标），不要太在意
        if (result != 0)
        {
            iconIndex = fileInfo.iIcon;
        }
    }

    cache[normalized] = iconIndex;
    return iconIndex;
}

// ============================================================================
// 磁盘缓存：key 组装、读入、落盘
// ============================================================================

std::wstring IconCache::Lowercase(std::wstring text)
{
    for (auto& character : text)
    {
        character = static_cast<wchar_t>(::towlower(character));
    }
    return text;
}

std::wstring IconCache::BuildDiskKey(const std::wstring& path, IconSize size,
                                     unsigned long long fileSize, const FILETIME& modified) const
{
    const unsigned long long stamp =
        (static_cast<unsigned long long>(modified.dwHighDateTime) << 32) |
        static_cast<unsigned long long>(modified.dwLowDateTime);

    const int side = (size == IconSize::Large) ? 48 : 16;

    wchar_t stampText[17] = {};
    ::swprintf_s(stampText, L"%016llx", stamp);

    std::wstring key = L"file|";
    key += Lowercase(path);
    key += L"|" + std::to_wstring(fileSize);
    key += L"|" + std::wstring(stampText);
    key += L"|" + std::to_wstring(side) + L"x" + std::to_wstring(side);
    key += L"|v1";
    return key;
}

HIMAGELIST IconCache::TargetImageList(IconSize size) const
{
    if (size == IconSize::Large)
    {
        return m_hLargeImageList;
    }
    if (size == IconSize::Medium)
    {
        return m_hMediumImageList;
    }
    return m_hImageList;
}

int IconCache::LoadIconFromDisk(const std::wstring& key, IconSize size)
{
    if (m_diskCache == nullptr)
    {
        return -1;
    }

    IconDiskCache::Entry entry;
    if (!m_diskCache->Load(IconDiskCache::Kind::Icon, key, entry))
    {
        return -1;
    }

    if (entry.negative)
    {
        return -2;
    }

    HICON icon = CreateIconFromBits(entry.width, entry.height, entry.pixels);
    if (icon == nullptr)
    {
        return -1;
    }

    HIMAGELIST target = TargetImageList(size);
    if (target == nullptr)
    {
        ::DestroyIcon(icon);
        return -1;
    }

    // 追加进图像列表拿索引（-1 表示"不替换，直接加一个"）
    const int index = ::ImageList_ReplaceIcon(target, -1, icon);
    ::DestroyIcon(icon);

    return (index >= 0) ? index : -1;
}

void IconCache::SaveIconToDisk(const std::wstring& key, HICON icon)
{
    if (m_diskCache == nullptr || icon == nullptr)
    {
        return;
    }

    std::vector<BYTE> pixels;
    int width  = 0;
    int height = 0;
    if (!ExtractIconBits(icon, pixels, width, height))
    {
        return;
    }

    IconDiskCache::Entry entry;
    entry.width  = width;
    entry.height = height;
    entry.pixels = std::move(pixels);

    m_diskCache->Save(IconDiskCache::Kind::Icon, key, entry);
}

// ============================================================================
// 按绝对 PIDL 取图标（控制面板那类「字符串解析不回来」的项专用）
// ============================================================================

int IconCache::GetIconIndexByPidl(const std::vector<unsigned char>& pidl, IconSize size)
{
    if (!m_initialized || pidl.empty())
    {
        return 0;
    }

    const bool  large    = (size == IconSize::Large);
    const DWORD sizeFlag = large ? SHGFI_LARGEICON : SHGFI_SMALLICON;

    SHFILEINFOW fileInfo = {};

    // pidl 的字节就是绝对 PIDL 本身，SHGetFileInfo 收的就是这根指针
    DWORD_PTR result = ::SHGetFileInfoW(
        reinterpret_cast<LPCWSTR>(pidl.data()),
        0,
        &fileInfo,
        sizeof(fileInfo),
        SHGFI_PIDL | SHGFI_SYSICONINDEX | sizeFlag);

    return (result != 0) ? fileInfo.iIcon : 0;
}

// ============================================================================
// 半透明图标（被剪切的条目用）
// ============================================================================

HICON IconCache::CreateDimmedIcon(HICON source, int size)
{
    if (source == nullptr || size <= 0)
    {
        return nullptr;
    }

    // 32 位带 alpha 的位图，图标才能保住透明边缘
    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth       = size;
    bitmapInfo.bmiHeader.biHeight      = -size;   // 负数 = 从上往下，省得上下颠倒
    bitmapInfo.bmiHeader.biPlanes      = 1;
    bitmapInfo.bmiHeader.biBitCount    = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void*    pixels   = nullptr;
    HDC      screenDc = ::GetDC(nullptr);
    HBITMAP  bitmap   = ::CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);

    if (bitmap == nullptr || pixels == nullptr)
    {
        if (bitmap != nullptr)
        {
            ::DeleteObject(bitmap);
        }
        ::ReleaseDC(nullptr, screenDc);
        return nullptr;
    }

    HDC     memoryDc = ::CreateCompatibleDC(screenDc);
    HBITMAP oldBitmap = static_cast<HBITMAP>(::SelectObject(memoryDc, bitmap));

    // 先把原图标画进去（含 alpha 通道）
    ::DrawIconEx(memoryDc, 0, 0, source, size, size, 0, nullptr, DI_NORMAL);

    ::SelectObject(memoryDc, oldBitmap);
    ::DeleteDC(memoryDc);
    ::ReleaseDC(nullptr, screenDc);

    // 把每个像素的 alpha 减半，就得到"淡了一半"的效果
    uint32_t* pixelData = static_cast<uint32_t*>(pixels);
    int       total     = size * size;

    bool hasAlpha = false;
    for (int i = 0; i < total; ++i)
    {
        uint32_t alpha = (pixelData[i] >> 24) & 0xFF;
        if (alpha != 0)
        {
            hasAlpha = true;
            pixelData[i] = (pixelData[i] & 0x00FFFFFF) |
                           (static_cast<uint32_t>(alpha / 2) << 24);
        }
    }

    // 图标根本没有 alpha 通道（老式图标），淡化没有意义，直接放弃
    if (!hasAlpha)
    {
        ::DeleteObject(bitmap);
        return nullptr;
    }

    // 位图转回 HICON
    ICONINFO iconInfo = {};
    iconInfo.fIcon    = TRUE;
    iconInfo.hbmColor = bitmap;
    iconInfo.hbmMask  = ::CreateBitmap(size, size, 1, 1, nullptr);

    HICON result = ::CreateIconIndirect(&iconInfo);

    if (iconInfo.hbmMask != nullptr)
    {
        ::DeleteObject(iconInfo.hbmMask);
    }
    ::DeleteObject(bitmap);

    return result;
}

int IconCache::GetDimmedIconIndex(int originalIndex, IconSize size)
{
    // Small 用 16px 那套；Medium(32) 和 Large(48) 共用大图标那套查询与索引
    const bool large = (size != IconSize::Small);

    // 大图标列表没拿到时，不能往空句柄里加图标，直接原样返回
    HIMAGELIST     targetList  = large ? m_hLargeImageList : m_hImageList;
    IImageList*    sourceList  = large ? m_largeSystemImageList.get() : m_systemImageList.get();
    auto&          dimmedCache = large ? m_largeDimmedIconCache : m_dimmedIconCache;

    if (!m_initialized || targetList == nullptr || sourceList == nullptr || originalIndex < 0)
    {
        return originalIndex;
    }

    auto cached = dimmedCache.find(originalIndex);
    if (cached != dimmedCache.end())
    {
        return cached->second;
    }

    int sizeX = large ? 48 : 16;
    int sizeY = sizeX;
    ::ImageList_GetIconSize(targetList, &sizeX, &sizeY);

    HICON original = nullptr;
    sourceList->GetIcon(originalIndex, ILD_TRANSPARENT, &original);

    if (original == nullptr)
    {
        return originalIndex;
    }

    HICON dimmed = CreateDimmedIcon(original, sizeX);
    ::DestroyIcon(original);

    if (dimmed == nullptr)
    {
        return originalIndex;
    }

    // 追加进同一个图像列表，列表控件的图标索引就能直接用了
    // ImageList_ReplaceIcon 传 -1 表示"不替换，直接加一个"，
    // 返回新图标的索引；失败返回 -1。
    // （IImageList::Add 只收位图不收图标，所以这里用 comctl32 的 C 接口）
    int newIndex = ::ImageList_ReplaceIcon(targetList, -1, dimmed);
    ::DestroyIcon(dimmed);

    if (newIndex < 0)
    {
        return originalIndex;
    }

    dimmedCache[originalIndex] = newIndex;
    return newIndex;
}

// ============================================================================
// 主图标 + overlay 角标合成
// ============================================================================

HICON IconCache::CreateCompositeIcon(HICON base, HICON overlay, int size)
{
    if (base == nullptr || overlay == nullptr || size <= 0)
    {
        return nullptr;
    }

    // 32 位带 alpha 的位图，保住主图标与角标的透明边缘
    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth       = size;
    bitmapInfo.bmiHeader.biHeight      = -size;
    bitmapInfo.bmiHeader.biPlanes      = 1;
    bitmapInfo.bmiHeader.biBitCount    = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void*    pixels   = nullptr;
    HDC      screenDc = ::GetDC(nullptr);
    HBITMAP  bitmap   = ::CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);

    if (bitmap == nullptr || pixels == nullptr)
    {
        if (bitmap != nullptr)
        {
            ::DeleteObject(bitmap);
        }
        ::ReleaseDC(nullptr, screenDc);
        return nullptr;
    }

    HDC     memoryDc  = ::CreateCompatibleDC(screenDc);
    HBITMAP oldBitmap = static_cast<HBITMAP>(::SelectObject(memoryDc, bitmap));

    // 先画主图标（满幅）
    ::DrawIconEx(memoryDc, 0, 0, base, size, size, 0, nullptr, DI_NORMAL);

    // overlay 角标贴左下角（横坐标贴左、纵坐标贴下）。
    // 边长按主图标档位取：小图标/列表/详细信息(16) 16px，大图标/平铺(48) 32px；
    // 贴着角标资源原生尺寸（约 16/32）以内，避免放大拉伸模糊。
    int overlaySize = (size <= 16) ? 16 : 32;
    if (overlaySize > size)
    {
        overlaySize = size;
    }
    const int overlayX = 0;
    const int overlayY = size - overlaySize;
    ::DrawIconEx(memoryDc, overlayX, overlayY, overlay, overlaySize, overlaySize,
                 0, nullptr, DI_NORMAL);

    ::SelectObject(memoryDc, oldBitmap);
    ::DeleteDC(memoryDc);
    ::ReleaseDC(nullptr, screenDc);

    ICONINFO iconInfo = {};
    iconInfo.fIcon    = TRUE;
    iconInfo.hbmColor = bitmap;
    iconInfo.hbmMask  = ::CreateBitmap(size, size, 1, 1, nullptr);

    HICON result = ::CreateIconIndirect(&iconInfo);

    if (iconInfo.hbmMask != nullptr)
    {
        ::DeleteObject(iconInfo.hbmMask);
    }
    ::DeleteObject(bitmap);

    return result;
}

int IconCache::GetCompositeIconIndex(int baseIndex, HICON overlay, IconSize size)
{
    if (overlay == nullptr || baseIndex < 0)
    {
        return baseIndex;
    }

    const bool large = (size != IconSize::Small);

    HIMAGELIST targetList = large ? m_hLargeImageList : m_hImageList;
    auto&      compCache  = large ? m_largeOverlayCompositeCache : m_overlayCompositeCache;

    if (!m_initialized || targetList == nullptr)
    {
        return baseIndex;
    }

    const auto key = std::make_pair(baseIndex, static_cast<const void*>(overlay));
    auto cached = compCache.find(key);
    if (cached != compCache.end())
    {
        return cached->second;
    }

    int sizeX = large ? 48 : 16;
    int sizeY = sizeX;
    ::ImageList_GetIconSize(targetList, &sizeX, &sizeY);

    // baseIndex 可能是系统原始索引，也可能是已追加进列表的淡化索引，
    // 统一从目标列表取图标。
    HICON baseIcon = ::ImageList_GetIcon(targetList, baseIndex, ILD_TRANSPARENT);

    if (baseIcon == nullptr)
    {
        return baseIndex;
    }

    HICON composite = CreateCompositeIcon(baseIcon, overlay, sizeX);
    ::DestroyIcon(baseIcon);

    if (composite == nullptr)
    {
        return baseIndex;
    }

    int newIndex = ::ImageList_ReplaceIcon(targetList, -1, composite);
    ::DestroyIcon(composite);

    if (newIndex < 0)
    {
        return baseIndex;
    }

    compCache[key] = newIndex;
    return newIndex;
}

} // namespace shell
