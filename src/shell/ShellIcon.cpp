// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ShellIcon.cpp
//
// 图标缓存实现。
// ============================================================================

#include "ShellIcon.h"

#include <cwctype>
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
int QueryIconIndexByExtension(const std::wstring& extension)
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
        SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);

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
int QueryFolderIconIndex()
{
    SHFILEINFOW fileInfo = {};
    DWORD_PTR result = ::SHGetFileInfoW(
        L"folder",
        FILE_ATTRIBUTE_DIRECTORY,
        &fileInfo,
        sizeof(fileInfo),
        SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);

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
    // m_systemImageList 由 wil::com_ptr 自动释放
    m_hImageList = nullptr;
}

bool IconCache::Initialize()
{
    if (m_initialized)
    {
        return true;
    }

    // SHIL_SMALL：系统小图标列表（列表视图用这个尺寸）
    // 直接把 IImageList 当 HIMAGELIST 用是官方支持的用法
    HRESULT hr = ::SHGetImageList(SHIL_SMALL, IID_PPV_ARGS(m_systemImageList.put()));
    if (FAILED(hr))
    {
        return false;
    }

    m_hImageList = reinterpret_cast<HIMAGELIST>(m_systemImageList.get());

    // 文件夹图标只查一次，之后所有文件夹共用
    m_folderIconIndex = QueryFolderIconIndex();

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

int IconCache::GetIconIndex(const std::wstring& name, bool isFolder)
{
    if (!m_initialized)
    {
        return 0;
    }

    // 文件夹：统一图标，零查询
    if (isFolder)
    {
        return m_folderIconIndex;
    }

    // 文件：按扩展名查缓存
    std::wstring extension = ExtractExtension(name);

    auto iterator = m_extensionIconCache.find(extension);
    if (iterator != m_extensionIconCache.end())
    {
        return iterator->second;
    }

    int iconIndex = QueryIconIndexByExtension(extension);
    m_extensionIconCache[extension] = iconIndex;
    return iconIndex;
}

int IconCache::GetIconIndexByPath(const std::wstring& path)
{
    if (!m_initialized || path.empty())
    {
        return 0;
    }

    SHFILEINFOW fileInfo = {};
    DWORD_PTR result = ::SHGetFileInfoW(
        path.c_str(),
        FILE_ATTRIBUTE_DIRECTORY,
        &fileInfo,
        sizeof(fileInfo),
        SHGFI_SYSICONINDEX | SHGFI_SMALLICON);

    if (result == 0)
    {
        return m_folderIconIndex;
    }
    return fileInfo.iIcon;
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

int IconCache::GetDimmedIconIndex(int originalIndex)
{
    if (!m_initialized || m_systemImageList == nullptr || originalIndex < 0)
    {
        return originalIndex;
    }

    auto cached = m_dimmedIconCache.find(originalIndex);
    if (cached != m_dimmedIconCache.end())
    {
        return cached->second;
    }

    int sizeX = 16;
    int sizeY = 16;
    ::ImageList_GetIconSize(m_hImageList, &sizeX, &sizeY);

    HICON original = nullptr;
    if (FAILED(m_systemImageList->GetIcon(originalIndex, ILD_TRANSPARENT, &original)) ||
        original == nullptr)
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
    int newIndex = ::ImageList_ReplaceIcon(m_hImageList, -1, dimmed);
    ::DestroyIcon(dimmed);

    if (newIndex < 0)
    {
        return originalIndex;
    }

    m_dimmedIconCache[originalIndex] = newIndex;
    return newIndex;
}

} // namespace shell
