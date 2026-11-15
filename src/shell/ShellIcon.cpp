// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ShellIcon.cpp
//
// 图标缓存实现。
// ============================================================================

#include "ShellIcon.h"

#include <cwctype>
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

} // namespace

namespace shell
{

IconCache::IconCache() = default;

IconCache::~IconCache()
{
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

int IconCache::GetIconIndexByFullPath(const std::wstring& path, IconSize size)
{
    if (!m_initialized || path.empty())
    {
        return 0;
    }

    // Small 用 16px 那套；Medium(32) 和 Large(48) 共用大图标那套查询与索引
    const bool large = (size != IconSize::Small);

    // 「C:」这种光秃秃的盘符要补成「C:\」：不补的话系统会把它当成"当前目录"，
    // 取回来的不是驱动器图标
    std::wstring normalized = path;
    if (normalized.size() == 2 && normalized[1] == L':')
    {
        normalized += L'\\';
    }

    auto& cache = large ? m_largePathIconCache : m_pathIconCache;

    auto cached = cache.find(normalized);
    if (cached != cache.end())
    {
        return cached->second;
    }

    int iconIndex = 0;
    const DWORD sizeFlag = large ? SHGFI_LARGEICON : SHGFI_SMALLICON;

    // Shell 命名空间里的虚拟项（「::{GUID}…」「shell:…」这种解析名）
    // 千万不能直接把字符串丢给 SHGetFileInfo：实测它一律返回 0，
    // 结果就是主文件夹 / 控制面板 / 网络 / 回收站全都显示成普通黄色文件夹。
    // 正确做法是先 SHParseDisplayName 换成 PIDL，再用 SHGFI_PIDL 问图标。
    const bool isShellNamespace = (normalized.compare(0, 2, L"::") == 0) ||
                                  (normalized.compare(0, 6, L"shell:") == 0);

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

} // namespace shell
