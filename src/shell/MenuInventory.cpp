// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/MenuInventory.cpp
//
// 新方案的核心采集器：逐个 handler 单独 QueryContextMenu，谁产出的项就
// 记到谁名下。这样归属来自“实际调用”，不来自菜单文字。
//
// 一期实现范围：
//   · 文件扩展名（单选文件）的 ShellExt + 静态 verb；
//   · 每项记录 owner（CLSID/DLL/应用名）和 invoke（点击怎么执行）；
//   · JSON 缓存，供后续零成本弹出和配置界面读取。
// ============================================================================

#include "shell/MenuInventory.h"
#include "shell/IconPaths.h"

#include "core/Logger.h"   // 动态组 owner 缓存要枚举扩展名清单

#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <shellapi.h>
#include <strsafe.h>
#include <winver.h>

#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

namespace shell
{

// 枚举 HKCR 下所有 ".xxx" 扩展名。
// 旧方案这里调 MenuCensus::EnumerateExtensions；新方案只保留 owner 枚举，
// 不需要整套 census，所以收成这个小函数。
std::vector<std::wstring> EnumerateExtensions()
{
    std::vector<std::wstring> result;

    HKEY root = nullptr;
    if (::RegOpenKeyExW(HKEY_CLASSES_ROOT, L"", 0, KEY_READ, &root) != ERROR_SUCCESS)
    {
        return result;
    }

    wchar_t name[256] = {};
    for (DWORD index = 0; ; ++index)
    {
        DWORD length = static_cast<DWORD>(std::size(name));
        const LSTATUS status = ::RegEnumKeyExW(root, index, name, &length,
                                               nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) continue;

        const std::wstring key(name, length);
        if (key.size() > 1 && key.front() == L'.')
        {
            result.push_back(key);
        }
    }

    ::RegCloseKey(root);
    std::sort(result.begin(), result.end());
    return result;
}

namespace
{

using nlohmann::json;
using Microsoft::WRL::ComPtr;

const int kInventoryVersion = 2;

// 菜单查询阶段传给 shell 扩展的代表项上限（兜底）。对齐 Windows 官方惯例：
// 查询菜单时只传少量代表项让扩展判断是否出菜单项，完整选择留到点击执行
// （InvokeCommand / Invoke）时再按完整选择重建上下文。避免悲观型扩展
// （如 WPS）拿到全部选中项后逐项绑定、随文件数线性变慢。
// 挑选规则见 SelectQueryRepresentatives：先按扩展名去重每种留一个；不足 3 个时
// 用占比最多的扩展名补齐到 3（避免塌成单选菜单导致命令偏移错位）；超过 16 再截断。
constexpr size_t kQuerySelectionLimit = 16;

// ---------------------------------------------------------------------------
// 字符串 / 路径 / 注册表小工具
// ---------------------------------------------------------------------------

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty()) return std::string();
    int length = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                       static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return std::string();
    std::string result(static_cast<size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                          result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(const std::string& text)
{
    if (text.empty()) return std::wstring();
    int length = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                       static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return std::wstring();
    std::wstring result(static_cast<size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                          result.data(), length);
    return result;
}

std::wstring ToLower(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return text;
}

// 查询阶段挑选传给 shell 扩展的代表项：
//   1) 先按扩展名去重，每种扩展名保留原顺序第一个出现的文件；
//   2) 不足 kMinRepresentatives 个时，用占比最多的扩展名（主类型）后续文件补齐；
//      纯类型多选若只留 1 个，扩展会按“单选”布局菜单，和执行阶段的多选菜单
//      命令排列不一致，导致命令偏移错位，所以保底凑够多选数量；
//   3) 超过 limit 再截断到前 limit 个。执行阶段不受此限制，仍传完整选择。
std::vector<std::wstring> SelectQueryRepresentatives(
    const std::vector<std::wstring>& paths, size_t limit)
{
    constexpr size_t kMinRepresentatives = 3;

    // 第一遍：统计各扩展名数量，找出占比最多的主类型（并列时取先出现的）。
    std::map<std::wstring, size_t> extCount;
    std::wstring mainExt;
    size_t mainCount = 0;
    for (const std::wstring& path : paths)
    {
        const wchar_t* ext = ::PathFindExtensionW(path.c_str());
        const std::wstring key = ToLower(ext == nullptr ? L"" : ext);
        ++extCount[key];
        if (extCount[key] > mainCount)
        {
            mainCount = extCount[key];
            mainExt = key;
        }
    }

    // 第二遍：按扩展名去重，每种取第一个。
    std::vector<std::wstring> reps;
    std::set<std::wstring> seenExt;
    std::set<std::wstring> addedPaths;

    for (const std::wstring& path : paths)
    {
        const wchar_t* ext = ::PathFindExtensionW(path.c_str());
        const std::wstring key = ToLower(ext == nullptr ? L"" : ext);

        if (!seenExt.insert(key).second) continue;  // 该扩展名已留过代表，跳过
        reps.push_back(path);
        addedPaths.insert(path);
        if (reps.size() >= limit) break;
    }

    // 第三遍：不足保底数量时，用主类型的后续文件补齐。
    // 若总文件数 < 3 本就全取；总文件数 >= 3 时主类型至少有 2 个，不会缺额。
    if (reps.size() < kMinRepresentatives)
    {
        for (const std::wstring& path : paths)
        {
            if (reps.size() >= kMinRepresentatives) break;
            if (addedPaths.count(path) != 0) continue;

            const wchar_t* ext = ::PathFindExtensionW(path.c_str());
            const std::wstring key = ToLower(ext == nullptr ? L"" : ext);
            if (key != mainExt) continue;

            reps.push_back(path);
            addedPaths.insert(path);
        }
    }

    // 兜底上限：种类再多也只传前 limit 个代表。
    if (reps.size() > limit) reps.resize(limit);
    return reps;
}

std::wstring TrimAmpersand(std::wstring text)
{
    // 菜单文字的 & 是助记符；只去掉单独的 &，保留 &&（原义 &）。
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
            }
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

// 去掉菜单文字末尾的快捷键标记，例如“打开方式(H)...”“在终端中打开(T)”。
// 去掉快捷键后保留原来的省略号；只识别 1~3 个 ASCII 字母/数字的括号，
// 避免误删“压缩(zipped)”这种真实括号。
std::wstring CleanMenuTitle(std::wstring text)
{
    auto trimSpace = [](std::wstring& value)
    {
        while (!value.empty() && (value.front() == L' ' || value.front() == L'\t'))
        {
            value.erase(value.begin());
        }
        while (!value.empty() && (value.back() == L' ' || value.back() == L'\t'))
        {
            value.pop_back();
        }
    };

    trimSpace(text);

    bool hasEllipsis = false;
    if (text.size() >= 3 && text.compare(text.size() - 3, 3, L"...") == 0)
    {
        text.erase(text.size() - 3);
        hasEllipsis = true;
        trimSpace(text);
    }
    else if (!text.empty() && text.back() == L'\x2026')   // 单字符省略号 …
    {
        text.pop_back();
        hasEllipsis = true;
        trimSpace(text);
    }

    if (text.size() >= 3 && text.back() == L')')
    {
        const size_t open = text.find_last_of(L'(');
        if (open != std::wstring::npos && open + 1 < text.size() - 1)
        {
            const size_t innerLength = text.size() - open - 2;
            bool accelerator = innerLength > 0 && innerLength <= 3;
            for (size_t i = open + 1; accelerator && i < text.size() - 1; ++i)
            {
                const wchar_t ch = text[i];
                const bool asciiLetter = (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
                const bool asciiDigit = (ch >= L'0' && ch <= L'9');
                if (!asciiLetter && !asciiDigit) accelerator = false;
            }
            if (accelerator)
            {
                text.erase(open);
                trimSpace(text);
            }
        }
    }

    if (hasEllipsis) text += L"...";
    return text;
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

// ---------------------------------------------------------------------------
// 动态菜单项图标落盘
//
// 二级菜单是展开时现场采集的，采集到的是 GDI 位图/HICON，不能直接跨次使用；
// 这里统一存成 16x16 BMP，右键绘制时再按文件名加载。文件名用稳定散列，
// 同一菜单项重复展开不会反复写盘。
// ---------------------------------------------------------------------------

namespace
{

std::wstring StableMenuIconHash(const std::wstring& text)
{
    unsigned int hash = 2166136261u;
    for (wchar_t ch : text)
    {
        hash ^= static_cast<unsigned int>(ch);
        hash *= 16777619u;
    }
    wchar_t buffer[16] = {};
    ::swprintf_s(buffer, L"%08x", hash);
    return buffer;
}

bool SaveMenuBitmapAsBmp(HBITMAP bitmap, int size, const std::wstring& path)
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
    if (canvas == nullptr || bits == nullptr) return false;

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

    const unsigned char* pixels = static_cast<const unsigned char*>(bits);
    bool blank = true;
    for (int i = 0; i < size * size * 4; i += 4)
    {
        if (pixels[i] != 0xFF || pixels[i + 1] != 0xFF || pixels[i + 2] != 0xFF)
        {
            blank = false;
            break;
        }
    }

    bool ok = false;
    if (!blank)
    {
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
            ok = ::WriteFile(file, &fileHeader, sizeof(fileHeader), &written, nullptr)
                 && ::WriteFile(file, &dibInfo.bmiHeader, sizeof(BITMAPINFOHEADER), &written, nullptr)
                 && ::WriteFile(file, bits, imageSize, &written, nullptr);
            ::CloseHandle(file);
        }
    }

    ::SelectObject(memDc, oldCanvas);
    ::DeleteDC(memDc);
    ::DeleteObject(canvas);
    return ok;
}

// ---------------------------------------------------------------------------
// 动态菜单项图标落盘（32 位 alpha ICO）
//
// 不再把图标存成普通 BMP 再猜背景色：BMP 没有透明通道，黑底/白底/抗锯齿
// 边缘都会留下脏边。这里统一生成带 alpha 的 ICO：
//   · 彩色位图 32bpp，alpha=0 的像素真正透明；
//   · 同时写 1bpp AND 蒙版，老系统按蒙版走也不会出现色块；
//   · 加载时直接用 LoadImage(IMAGE_ICON) 得到 HICON，不再做颜色键。
// ---------------------------------------------------------------------------

bool MenuPixelIsBackground(const unsigned char* pixel, COLORREF color, int tolerance)
{
    const int dr = std::abs(static_cast<int>(pixel[2]) - static_cast<int>(GetRValue(color)));
    const int dg = std::abs(static_cast<int>(pixel[1]) - static_cast<int>(GetGValue(color)));
    const int db = std::abs(static_cast<int>(pixel[0]) - static_cast<int>(GetBValue(color)));
    return dr <= tolerance && dg <= tolerance && db <= tolerance;
}

COLORREF MenuPixelsBackgroundColor(const unsigned char* pixels)
{
    // 取四角占多数的颜色。背景可能是黑、白或主题色；只用于“没有 alpha”
    // 的旧式位图兜底，正常带 alpha 的图标不会走这里。
    const int corners[4][2] = { { 0, 0 }, { 15, 0 }, { 0, 15 }, { 15, 15 } };
    COLORREF colors[4] = {};
    for (int i = 0; i < 4; ++i)
    {
        const int offset = (corners[i][1] * 16 + corners[i][0]) * 4;
        colors[i] = RGB(pixels[offset + 2], pixels[offset + 1], pixels[offset]);
    }

    COLORREF best = RGB(255, 255, 255);
    int bestCount = 0;
    for (COLORREF color : colors)
    {
        int count = 0;
        for (COLORREF other : colors)
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

bool RemoveMenuPixelsBackground(unsigned char* pixels)
{
    // 只从四条边向内 flood fill，避免把图标内部的黑色细节当成背景挖掉。
    // 适用于没有 alpha 的旧式菜单位图（DDB / 扩展用 GDI 直接画的情况）。
    const COLORREF background = MenuPixelsBackgroundColor(pixels);
    bool transparent[16 * 16] = {};
    int queue[16 * 16] = {};
    int head = 0;
    int tail = 0;

    auto push = [&](int x, int y)
    {
        const int index = y * 16 + x;
        if (transparent[index]) return;
        const int offset = index * 4;
        if (!MenuPixelIsBackground(&pixels[offset], background, 10)) return;
        transparent[index] = true;
        queue[tail++] = index;
    };

    for (int x = 0; x < 16; ++x)
    {
        push(x, 0);
        push(x, 15);
    }
    for (int y = 0; y < 16; ++y)
    {
        push(0, y);
        push(15, y);
    }

    while (head < tail)
    {
        const int index = queue[head++];
        const int x = index % 16;
        const int y = index / 16;
        if (x > 0) push(x - 1, y);
        if (x < 15) push(x + 1, y);
        if (y > 0) push(x, y - 1);
        if (y < 15) push(x, y + 1);
    }

    int count = 0;
    for (int i = 0; i < 16 * 16; ++i)
    {
        if (transparent[i]) ++count;
    }
    if (count == 16 * 16) return false; // 整张都是背景，说明没采到图标

    for (int i = 0; i < 16 * 16; ++i)
    {
        if (transparent[i]) pixels[i * 4 + 3] = 0;
    }
    return true;
}

bool CaptureMenuIconPixels(HBITMAP bitmap, int cropX, unsigned char* pixels)
{
    if (bitmap == nullptr || bitmap == HBMMENU_CALLBACK) return false;

    BITMAP info = {};
    if (::GetObjectW(bitmap, sizeof(info), &info) != sizeof(info) ||
        info.bmWidth <= 0 || info.bmHeight <= 0)
    {
        return false;
    }

    // 先把源位图读成 32bpp 顶朝下的 BGRA；owner-draw 的 32bpp DIB 会保留
    // 扩展画出来的 alpha，普通 DDB 的 alpha 则是 0，后面走 flood fill。
    std::vector<unsigned char> raw(
        static_cast<size_t>(info.bmWidth) * static_cast<size_t>(info.bmHeight) * 4);

    BITMAPINFO dibInfo = {};
    dibInfo.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    dibInfo.bmiHeader.biWidth       = info.bmWidth;
    dibInfo.bmiHeader.biHeight      = -info.bmHeight;
    dibInfo.bmiHeader.biPlanes      = 1;
    dibInfo.bmiHeader.biBitCount    = 32;
    dibInfo.bmiHeader.biCompression = BI_RGB;

    HDC dc = ::GetDC(nullptr);
    const int lines = ::GetDIBits(dc, bitmap, 0, static_cast<UINT>(info.bmHeight),
                                  raw.data(), &dibInfo, DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, dc);
    if (lines <= 0) return false;

    bool hasZeroAlpha = false;
    bool hasOpaqueAlpha = false;
    for (size_t i = 3; i < raw.size(); i += 4)
    {
        if (raw[i] == 0) hasZeroAlpha = true;
        else hasOpaqueAlpha = true;
    }
    const bool alphaValid = hasZeroAlpha && hasOpaqueAlpha;

    for (int y = 0; y < 16; ++y)
    {
        for (int x = 0; x < 16; ++x)
        {
            int sourceX = cropX + x;
            int sourceY = y;
            if (sourceX >= info.bmWidth || sourceY >= info.bmHeight)
            {
                // 尺寸不是 16x16 时按比例缩放采样。
                sourceX = x * info.bmWidth / 16;
                sourceY = y * info.bmHeight / 16;
            }
            sourceX = (std::min)(sourceX, static_cast<int>(info.bmWidth) - 1);
            sourceY = (std::min)(sourceY, static_cast<int>(info.bmHeight) - 1);

            const size_t source = (static_cast<size_t>(sourceY) * info.bmWidth + sourceX) * 4;
            const int target = (y * 16 + x) * 4;
            pixels[target]     = raw[source];
            pixels[target + 1] = raw[source + 1];
            pixels[target + 2] = raw[source + 2];
            pixels[target + 3] = alphaValid ? raw[source + 3] : 0xFF;
        }
    }

    if (!alphaValid && !RemoveMenuPixelsBackground(pixels))
    {
        return false;
    }
    return true;
}

bool WriteMenuIconIco(const std::wstring& path, const unsigned char* pixels)
{
#pragma pack(push, 1)
    struct IcoDir
    {
        WORD reserved;
        WORD type;
        WORD count;
    };
    struct IcoEntry
    {
        BYTE  width;
        BYTE  height;
        BYTE  colorCount;
        BYTE  reserved;
        WORD  planes;
        WORD  bitCount;
        DWORD bytesInRes;
        DWORD imageOffset;
    };
#pragma pack(pop)

    IcoDir dir = { 0, 1, 1 };
    IcoEntry entry = {};
    entry.width       = 16;
    entry.height      = 16;
    entry.planes      = 1;
    entry.bitCount    = 32;
    entry.bytesInRes  = sizeof(BITMAPINFOHEADER) + 16 * 16 * 4 + 16 * 2;
    entry.imageOffset = sizeof(IcoDir) + sizeof(IcoEntry);

    BITMAPINFOHEADER header = {};
    header.biSize        = sizeof(BITMAPINFOHEADER);
    header.biWidth       = 16;
    header.biHeight      = 32; // XOR 16 行 + AND 16 行
    header.biPlanes      = 1;
    header.biBitCount    = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage   = 16 * 16 * 4;

    unsigned char xorData[16 * 16 * 4] = {};
    unsigned char andData[16 * 2] = {};

    for (int y = 0; y < 16; ++y)
    {
        for (int x = 0; x < 16; ++x)
        {
            const int source = (y * 16 + x) * 4;
            const int target = ((15 - y) * 16 + x) * 4;
            xorData[target]     = pixels[source];
            xorData[target + 1] = pixels[source + 1];
            xorData[target + 2] = pixels[source + 2];
            xorData[target + 3] = pixels[source + 3];

            if (pixels[source + 3] == 0)
            {
                // AND 蒙版：1 = 透明。32bpp alpha 图标会优先用 alpha，
                // 蒙版主要保证老路径也不会出现黑块。
                andData[(15 - y) * 2 + x / 8] |= static_cast<unsigned char>(1u << (7 - (x % 8)));
            }
        }
    }

    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    bool ok = true;
    DWORD written = 0;
    ok = ok && ::WriteFile(file, &dir, sizeof(dir), &written, nullptr);
    ok = ok && ::WriteFile(file, &entry, sizeof(entry), &written, nullptr);
    ok = ok && ::WriteFile(file, &header, sizeof(header), &written, nullptr);
    ok = ok && ::WriteFile(file, xorData, sizeof(xorData), &written, nullptr);
    ok = ok && ::WriteFile(file, andData, sizeof(andData), &written, nullptr);
    ::CloseHandle(file);

    if (!ok) ::DeleteFileW(path.c_str());
    return ok;
}

std::wstring SaveDualBackgroundIcon(HBITMAP blackBackground,
                                    HBITMAP whiteBackground,
                                    const std::wstring& key,
                                    int cropX = 0)
{
    if (blackBackground == nullptr || whiteBackground == nullptr) return std::wstring();

    const std::wstring dir = IconDir();
    if (dir.empty()) return std::wstring();

    const std::wstring name = L"m" + StableMenuIconHash(key) + L".ico";
    const std::wstring path = dir + L"\\" + name;
    if (::PathFileExistsW(path.c_str())) return name;

    unsigned char black[16 * 16 * 4] = {};
    unsigned char white[16 * 16 * 4] = {};
    if (!CaptureMenuIconPixels(blackBackground, cropX, black) ||
        !CaptureMenuIconPixels(whiteBackground, cropX, white))
    {
        return std::wstring();
    }

    // 同一图标分别画到纯黑、纯白两个背景上：
    //   Cb = F * a            （黑底）
    //   Cw = F * a + 255*(1-a)（白底）
    // 两式相减就能反推出 alpha 和前景色 F，抗锯齿边缘也能正确还原。
    unsigned char pixels[16 * 16 * 4] = {};
    for (int i = 0; i < 16 * 16; ++i)
    {
        const int offset = i * 4;
        int diff = 0;
        for (int channel = 0; channel < 3; ++channel)
        {
            diff = (std::max)(diff, static_cast<int>(white[offset + channel]) -
                                      static_cast<int>(black[offset + channel]));
        }
        const int alpha = 255 - diff;
        if (alpha <= 0)
        {
            pixels[offset + 3] = 0;
            continue;
        }

        for (int channel = 0; channel < 3; ++channel)
        {
            const int foreground = static_cast<int>(black[offset + channel]) * 255 / alpha;
            pixels[offset + channel] = static_cast<unsigned char>(
                (std::max)(0, (std::min)(255, foreground)));
        }
        pixels[offset + 3] = static_cast<unsigned char>((std::min)(255, alpha));
    }

    bool anyOpaque = false;
    for (int i = 3; i < 16 * 16 * 4; i += 4)
    {
        if (pixels[i] != 0) { anyOpaque = true; break; }
    }
    if (!anyOpaque) return std::wstring();

    return WriteMenuIconIco(path, pixels) ? name : std::wstring();
}

std::wstring SaveMenuIconBitmap(HBITMAP bitmap, const std::wstring& key, int cropX = 0)
{
    if (bitmap == nullptr || bitmap == HBMMENU_CALLBACK) return std::wstring();

    const std::wstring dir = IconDir();
    if (dir.empty()) return std::wstring();

    const std::wstring name = L"m" + StableMenuIconHash(key) + L".ico";
    const std::wstring path = dir + L"\\" + name;
    if (::PathFileExistsW(path.c_str())) return name;

    unsigned char pixels[16 * 16 * 4] = {};
    if (!CaptureMenuIconPixels(bitmap, cropX, pixels))
    {
        return std::wstring();
    }

    return WriteMenuIconIco(path, pixels) ? name : std::wstring();
}

HBITMAP RenderHIconOnColor(HICON icon, COLORREF background)
{
    if (icon == nullptr) return nullptr;

    BITMAPINFO dibInfo = {};
    dibInfo.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    dibInfo.bmiHeader.biWidth       = 16;
    dibInfo.bmiHeader.biHeight      = 16;
    dibInfo.bmiHeader.biPlanes      = 1;
    dibInfo.bmiHeader.biBitCount    = 32;
    dibInfo.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = ::GetDC(nullptr);
    HBITMAP canvas = ::CreateDIBSection(screen, &dibInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    if (canvas == nullptr || bits == nullptr) return nullptr;

    HDC memDc = ::CreateCompatibleDC(nullptr);
    HGDIOBJ old = ::SelectObject(memDc, canvas);
    RECT full = { 0, 0, 16, 16 };
    HBRUSH brush = ::CreateSolidBrush(background);
    ::FillRect(memDc, &full, brush);
    ::DeleteObject(brush);
    ::DrawIconEx(memDc, 0, 0, icon, 16, 16, 0, nullptr, DI_NORMAL);
    ::SelectObject(memDc, old);
    ::DeleteDC(memDc);
    return canvas;
}

std::wstring SaveMenuIconHIcon(HICON icon, const std::wstring& key)
{
    if (icon == nullptr) return std::wstring();

    const std::wstring dir = IconDir();
    if (dir.empty()) return std::wstring();

    const std::wstring name = L"m" + StableMenuIconHash(key) + L".ico";
    const std::wstring path = dir + L"\\" + name;
    if (::PathFileExistsW(path.c_str())) return name;

    HBITMAP black = RenderHIconOnColor(icon, RGB(0, 0, 0));
    HBITMAP white = RenderHIconOnColor(icon, RGB(255, 255, 255));
    const std::wstring result = SaveDualBackgroundIcon(black, white, key);
    if (black != nullptr) ::DeleteObject(black);
    if (white != nullptr) ::DeleteObject(white);
    return result;
}

HICON ExtractMenuIconFromSpec(const std::wstring& spec)
{
    std::wstring text = spec;
    if (!text.empty() && text.front() == L'@') text.erase(text.begin());
    if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"')
    {
        text = text.substr(1, text.size() - 2);
    }

    std::wstring filePart = text;
    int index = 0;
    const size_t comma = text.find_last_of(L',');
    if (comma != std::wstring::npos)
    {
        filePart = text.substr(0, comma);
        index = _wtoi(text.substr(comma + 1).c_str());
    }
    if (filePart.empty()) return nullptr;

    HICON largeIcon = nullptr;
    HICON smallIcon = nullptr;
    UINT count = ::ExtractIconExW(filePart.c_str(), index, &largeIcon, &smallIcon, 1);
    if (count == 0)
    {
        ::SHDefExtractIconW(filePart.c_str(), index, 0, &largeIcon, &smallIcon, MAKELONG(16, 16));
    }
    if (largeIcon != nullptr) ::DestroyIcon(largeIcon);
    return smallIcon;
}

HBITMAP RenderOwnerDrawIcon(HMENU menu, const MENUITEMINFO& info,
                            IContextMenu3* cm3, IContextMenu2* cm2,
                            int left, COLORREF background)
{
    if (menu == nullptr || (cm3 == nullptr && cm2 == nullptr)) return nullptr;

    BITMAPINFO dibInfo = {};
    dibInfo.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    dibInfo.bmiHeader.biWidth       = 32;      // 给 rcItem.left-16 和 rcItem.left 两种画法留空间
    dibInfo.bmiHeader.biHeight      = 16;
    dibInfo.bmiHeader.biPlanes      = 1;
    dibInfo.bmiHeader.biBitCount    = 32;
    dibInfo.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = ::GetDC(nullptr);
    HBITMAP canvas = ::CreateDIBSection(screen, &dibInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    if (canvas == nullptr || bits == nullptr) return nullptr;

    HDC memDc = ::CreateCompatibleDC(nullptr);
    HGDIOBJ old = ::SelectObject(memDc, canvas);
    RECT full = { 0, 0, 32, 16 };
    HBRUSH brush = ::CreateSolidBrush(background);
    ::FillRect(memDc, &full, brush);
    ::DeleteObject(brush);

    DRAWITEMSTRUCT dis = {};
    dis.CtlType    = ODT_MENU;
    dis.itemID     = info.wID;
    dis.itemAction = ODA_DRAWENTIRE;
    dis.itemState  = (info.fState & MFS_DISABLED) ? ODS_DISABLED : 0;
    dis.hwndItem   = reinterpret_cast<HWND>(menu);
    dis.hDC        = memDc;
    dis.rcItem     = { left, 0, left + 16, 16 };
    dis.itemData   = info.dwItemData;

    // 有些 owner-draw 扩展要先用 WM_MEASUREITEM 初始化自己的绘制数据，
    // 直接发 WM_DRAWITEM 会画空。先按 16x16 量一次。
    MEASUREITEMSTRUCT measure = {};
    measure.CtlType    = ODT_MENU;
    measure.itemID     = info.wID;
    measure.itemWidth  = 16;
    measure.itemHeight = 16;
    measure.itemData   = info.dwItemData;

    LRESULT result = 0;
    if (cm3 != nullptr)
    {
        cm3->HandleMenuMsg2(WM_MEASUREITEM, 0, reinterpret_cast<LPARAM>(&measure), &result);
    }
    else if (cm2 != nullptr)
    {
        cm2->HandleMenuMsg(WM_MEASUREITEM, 0, reinterpret_cast<LPARAM>(&measure));
    }

    HRESULT drawResult = E_FAIL;
    if (cm3 != nullptr)
    {
        drawResult = cm3->HandleMenuMsg2(
            WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&dis), &result);
    }
    // 有些扩展只实现了 IContextMenu2（WPS 这类老式 owner-draw），
    // IContextMenu3 不存在或返回失败时，退回 HandleMenuMsg 再画一次。
    if (cm2 != nullptr && (cm3 == nullptr || FAILED(drawResult)))
    {
        cm2->HandleMenuMsg(WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&dis));
    }

    ::SelectObject(memDc, old);
    ::DeleteDC(memDc);
    return canvas;
}

std::wstring CaptureMenuItemIcon(HMENU menu, const MENUITEMINFO& info,
                                 IContextMenu3* cm3, IContextMenu2* cm2,
                                 const std::wstring& key)
{
    if (info.hbmpItem != nullptr && info.hbmpItem != HBMMENU_CALLBACK)
    {
        return SaveMenuIconBitmap(info.hbmpItem, key);
    }

    // hbmpItem == HBMMENU_CALLBACK 或 MFT_OWNERDRAW：菜单项由扩展自己画。
    // 用 IContextMenu3::HandleMenuMsg2 模拟一次 WM_DRAWITEM，并且分别画到
    // 纯黑、纯白两个背景上，反推 alpha；这样不依赖扩展是否支持 alpha。
    if ((cm3 != nullptr || cm2 != nullptr) &&
        (info.hbmpItem == HBMMENU_CALLBACK || (info.fType & 0x00000100) != 0))
    {
        for (int left : { 0, 16 })
        {
            HBITMAP black = RenderOwnerDrawIcon(menu, info, cm3, cm2, left, RGB(0, 0, 0));
            HBITMAP white = RenderOwnerDrawIcon(menu, info, cm3, cm2, left, RGB(255, 255, 255));
            if (black == nullptr || white == nullptr)
            {
                if (black != nullptr) ::DeleteObject(black);
                if (white != nullptr) ::DeleteObject(white);
                continue;
            }

            for (int cropX : { 0, 16 })
            {
                const std::wstring name = SaveDualBackgroundIcon(black, white, key, cropX);
                if (!name.empty())
                {
                    ::DeleteObject(black);
                    ::DeleteObject(white);
                    return name;
                }
            }
            ::DeleteObject(black);
            ::DeleteObject(white);
        }
    }
    return std::wstring();
}

std::wstring CaptureExplorerCommandIcon(IExplorerCommand* command,
                                        IShellItemArray* items,
                                        const std::wstring& key)
{
    if (command == nullptr) return std::wstring();

    LPWSTR iconSpec = nullptr;
    if (FAILED(command->GetIcon(items, &iconSpec)) || iconSpec == nullptr)
    {
        return std::wstring();
    }

    std::wstring spec = iconSpec;
    ::CoTaskMemFree(iconSpec);

    HICON icon = ExtractMenuIconFromSpec(spec);
    if (icon == nullptr) return std::wstring();

    std::wstring name = SaveMenuIconHIcon(icon, key);
    ::DestroyIcon(icon);
    return name;
}

} // namespace

std::wstring NowText()
{
    SYSTEMTIME st = {};
    ::GetLocalTime(&st);
    wchar_t buffer[64] = {};
    ::swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

// 读注册表字符串；不存在返回空。
std::wstring RegGetString(HKEY root, const std::wstring& subKey, const wchar_t* valueName)
{
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return std::wstring();
    }

    std::wstring result;
    DWORD type = 0;
    DWORD size = 0;
    if (::RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && size >= sizeof(wchar_t))
    {
        result.resize(size / sizeof(wchar_t));
        DWORD read = size;
        if (::RegQueryValueExW(key, valueName, nullptr, &type,
                               reinterpret_cast<LPBYTE>(result.data()), &read) != ERROR_SUCCESS)
        {
            result.clear();
        }
        else
        {
            while (!result.empty() && result.back() == L'\0') result.pop_back();
        }
    }

    ::RegCloseKey(key);
    return result;
}

bool RegHasValue(HKEY root, const std::wstring& subKey, const wchar_t* valueName)
{
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    const bool exists = ::RegQueryValueExW(key, valueName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    ::RegCloseKey(key);
    return exists;
}

std::vector<std::wstring> RegSubKeys(HKEY root, const std::wstring& subKey)
{
    std::vector<std::wstring> keys;
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return keys;

    for (DWORD index = 0; ; ++index)
    {
        wchar_t name[512] = {};
        DWORD nameLength = static_cast<DWORD>(std::size(name));
        const LONG rc = ::RegEnumKeyExW(key, index, name, &nameLength,
                                        nullptr, nullptr, nullptr, nullptr);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS) break;
        if (nameLength > 0) keys.emplace_back(name, nameLength);
    }

    ::RegCloseKey(key);
    return keys;
}

// 注册表里的默认值可能是 @dll,-123 资源引用；先原样返回，解析失败时
// 至少还能显示 verb 名，不会因为资源失败丢掉整个菜单项。
std::wstring DisplayTextFromRegistry(const std::wstring& raw, const std::wstring& fallback)
{
    if (raw.empty() || raw[0] == L'@') return fallback;
    return TrimAmpersand(raw);
}

// 从命令行里取可执行文件路径（支持 "..." 和普通 token）。
std::wstring ExecutableFromCommand(const std::wstring& commandLine)
{
    if (commandLine.empty()) return std::wstring();

    std::wstring text = commandLine;
    const size_t first = text.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return std::wstring();
    text.erase(0, first);

    if (text[0] == L'"')
    {
        const size_t end = text.find(L'"', 1);
        if (end != std::wstring::npos) return text.substr(1, end - 1);
    }

    const size_t space = text.find_first_of(L" \t");
    return space == std::wstring::npos ? text : text.substr(0, space);
}

// 注册表/静态命令里经常是 %SystemRoot%\...，版本资源和文件检查前先展开。
std::wstring ExpandEnvironment(const std::wstring& text)
{
    if (text.empty()) return text;
    DWORD size = ::ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
    if (size == 0) return text;
    std::wstring result(size, L'\0');
    if (::ExpandEnvironmentStringsW(text.c_str(), result.data(), size) == 0) return text;
    while (!result.empty() && result.back() == L'\0') result.pop_back();
    return result;
}

// ---------------------------------------------------------------------------
// 版本资源：应用名/文件说明
// ---------------------------------------------------------------------------

struct FileVersionInfo
{
    std::wstring productName;
    std::wstring fileDescription;
    std::wstring companyName;
    std::wstring internalName;
};

FileVersionInfo ReadFileVersionInfo(const std::wstring& path)
{
    FileVersionInfo info;

    DWORD handle = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) return info;

    std::vector<BYTE> buffer(size);
    if (!::GetFileVersionInfoW(path.c_str(), 0, size, buffer.data())) return info;

    struct LangCodePage
    {
        WORD language;
        WORD codePage;
    };

    LangCodePage* translations = nullptr;
    UINT translationSize = 0;
    if (!::VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation",
                          reinterpret_cast<void**>(&translations), &translationSize) ||
        translations == nullptr || translationSize < sizeof(LangCodePage))
    {
        return info;
    }

    for (unsigned int index = 0; index + sizeof(LangCodePage) <= translationSize; index += sizeof(LangCodePage))
    {
        wchar_t subBlock[128] = {};
        ::swprintf_s(subBlock, L"\\StringFileInfo\\%04x%04x\\",
                     translations[index].language, translations[index].codePage);

        auto readString = [&](const wchar_t* name) -> std::wstring
        {
            wchar_t* value = nullptr;
            UINT valueSize = 0;
            if (::VerQueryValueW(buffer.data(), (std::wstring(subBlock) + name).c_str(),
                                 reinterpret_cast<void**>(&value), &valueSize) && value != nullptr)
            {
                // VerQueryValueW 返回的是以 NUL 结尾的宽字符串；valueSize 的单位在不同
                // 版本资源/语言块里并不稳定（WinRAR 7.20 这里按“字符数”算会把缓冲区
                // 后面的二进制一起读进来，应用名变成 "WinRAR\0\0D..."）。
                // 因此不再用 valueSize 截长度，直接按 NUL 结尾取，再清掉控制字符。
                std::wstring text(value);
                while (!text.empty() && text.back() < 0x20) text.pop_back();
                for (wchar_t& ch : text)
                {
                    if (ch < 0x20 || ch == 0x7f) ch = L' ';
                }
                return text;
            }
            return std::wstring();
        };

        if (info.productName.empty()) info.productName = readString(L"ProductName");
        if (info.fileDescription.empty()) info.fileDescription = readString(L"FileDescription");
        if (info.companyName.empty()) info.companyName = readString(L"CompanyName");
        if (info.internalName.empty()) info.internalName = readString(L"InternalName");
        if (!info.productName.empty() || !info.fileDescription.empty()) break;
    }

    return info;
}

std::wstring BestAppName(const std::wstring& serverPath, const std::wstring& fallback)
{
    if (serverPath.empty()) return fallback;

    // 包里的 DllPath 可能指向一个 sparse package 里并不存在的 stub（TortoiseSVN 就是）。
    // 文件都不存在时，版本资源一定读不到，直接用清单/包给的显示名，别退化成 DLL 文件名。
    if (!::PathFileExistsW(serverPath.c_str()) && !fallback.empty())
    {
        return fallback;
    }

    const FileVersionInfo info = ReadFileVersionInfo(serverPath);
    if (!info.productName.empty())
    {
        // 系统 DLL/EXE 的产品名统一成界面认识的“Windows 系统”，
        // 不把 shell32.dll / Explorer.exe 显示成一堆不同品牌。
        if (info.productName.find(L"Windows") != std::wstring::npos &&
            info.companyName.find(L"Microsoft") != std::wstring::npos)
        {
            return L"Windows 系统";
        }
        return info.productName;
    }
    if (!info.fileDescription.empty()) return info.fileDescription;
    if (!info.companyName.empty()) return info.companyName;

    wchar_t base[512] = {};
    ::StringCchCopyW(base, std::size(base), serverPath.c_str());
    ::PathRemoveExtensionW(base);
    ::PathStripPathW(base);
    return base[0] != L'\0' ? std::wstring(base) : fallback;
}

// ---------------------------------------------------------------------------
// CLSID → server / 注册位置
// ---------------------------------------------------------------------------

std::wstring ClsidServerPath(const std::wstring& clsid)
{
    const std::wstring base = L"CLSID\\" + clsid + L"\\";
    std::wstring server = RegGetString(HKEY_CLASSES_ROOT, base + L"InprocServer32", nullptr);
    if (server.empty())
    {
        server = RegGetString(HKEY_CLASSES_ROOT, base + L"LocalServer32", nullptr);
    }
    if (server.empty())
    {
        server = RegGetString(HKEY_CLASSES_ROOT, base + L"InprocServer32", L"ServerExecutable");
    }
    if (server.empty()) return std::wstring();

    server = ExpandEnvironment(server);
    // InprocServer32 的默认值通常已经是路径；如果是命令行，取 EXE。
    if (server.find(L' ') != std::wstring::npos && server[0] != L'"')
    {
        const std::wstring exe = ExecutableFromCommand(server);
        if (!exe.empty() && ::PathFileExistsW(exe.c_str())) return exe;
    }
    return server;
}

// ---------------------------------------------------------------------------
// 一个只含 CF_HDROP 的 IDataObject（构造失败时的兜底）
//
// 正常路径优先用 SHCreateDataObject：它会同时给 CF_HDROP 和 shell item list，
// TortoiseSVN 这类扩展要的是后者。这个类只在 SHCreateDataObject 失败时兜底，
// 保证至少还有 CF_HDROP 可用。
// ---------------------------------------------------------------------------

class FileDataObject final : public IDataObject
{
public:
    explicit FileDataObject(std::vector<std::wstring> paths)
        : m_paths(std::move(paths)), m_ref(1) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDataObject)
        {
            *ppv = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ::InterlockedIncrement(&m_ref); }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG ref = ::InterlockedDecrement(&m_ref);
        if (ref == 0) delete this;
        return ref;
    }

    // IDataObject：只实现 CF_HDROP，够 IShellExtInit 初始化用。
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override
    {
        if (format == nullptr || medium == nullptr) return E_POINTER;
        if (format->cfFormat != CF_HDROP || !(format->tymed & TYMED_HGLOBAL))
        {
            return DV_E_FORMATETC;
        }

        // DROPFILES 后面按「路径1\0路径2\0...\0」排列；最后再补一个 \0 结束整个列表。
        DWORD bytes = 0;
        for (const std::wstring& path : m_paths)
        {
            bytes += static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t));
        }
        bytes += static_cast<DWORD>(sizeof(wchar_t));

        const DWORD total = static_cast<DWORD>(sizeof(DROPFILES) + bytes);
        HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total);
        if (memory == nullptr) return E_OUTOFMEMORY;

        void* data = ::GlobalLock(memory);
        if (data == nullptr)
        {
            ::GlobalFree(memory);
            return E_OUTOFMEMORY;
        }

        DROPFILES* drop = static_cast<DROPFILES*>(data);
        drop->pFiles = sizeof(DROPFILES);
        drop->fWide = TRUE;
        wchar_t* target = reinterpret_cast<wchar_t*>(static_cast<BYTE*>(data) + sizeof(DROPFILES));
        for (const std::wstring& path : m_paths)
        {
            const size_t bytesPerPath = (path.size() + 1) * sizeof(wchar_t);
            ::memcpy(target, path.c_str(), bytesPerPath);
            target += path.size() + 1;
        }
        *target = L'\0';

        ::GlobalUnlock(memory);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
    {
        if (format == nullptr) return E_POINTER;
        return (format->cfFormat == CF_HDROP && (format->tymed & TYMED_HGLOBAL)) ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override
    {
        if (out != nullptr) out->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC** out) override
    {
        if (out != nullptr) *out = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return E_NOTIMPL; }

private:
    ~FileDataObject() = default;

    std::vector<std::wstring> m_paths;
    LONG                      m_ref;
};

// ---------------------------------------------------------------------------
// 真实 shell 扩展初始化上下文
//
// 旧代码只给 handler 一个自建 CF_HDROP data object，父文件夹 PIDL 传 nullptr。
// TortoiseSVN 这类扩展需要「父文件夹 PIDL + shell item list」，所以会什么都不出。
// 这里统一构造：
//   · 父文件夹 PIDL（IShellExtInit::Initialize 的第一个参数）
//   · SHCreateDataObject（带 CF_HDROP + shell item list）
// 任何路径都先走这套；只有 SHCreateDataObject 失败时才退回自建 CF_HDROP。
// ---------------------------------------------------------------------------

std::wstring ParentFolderOf(const std::wstring& path)
{
    if (path.empty()) return std::wstring();
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    if (slash == 0) return path.substr(0, 1);
    return path.substr(0, slash);
}

struct ShellExtContext
{
    PIDLIST_ABSOLUTE                      folder = nullptr;
    std::vector<PIDLIST_ABSOLUTE>         childPidls;   // 需要释放的绝对 PIDL
    std::vector<PCUITEMID_CHILD>          children;     // SHCreateDataObject 要的相对 PIDL
    ComPtr<IDataObject>                   dataObject;

    ~ShellExtContext()
    {
        if (folder != nullptr) ::CoTaskMemFree(folder);
        for (PIDLIST_ABSOLUTE child : childPidls) ::CoTaskMemFree(child);
    }
};

bool BuildShellExtContext(const std::vector<std::wstring>& paths,
                          const std::wstring& parentFolder,
                          ShellExtContext& context,
                          std::wstring* error)
{
    const std::wstring folderPath = !parentFolder.empty()
        ? parentFolder
        : (paths.empty() ? std::wstring() : ParentFolderOf(paths.front()));

    if (folderPath.empty())
    {
        if (error != nullptr) *error = L"无法确定父文件夹";
        return false;
    }

    {
        if (FAILED(::SHParseDisplayName(folderPath.c_str(), nullptr, &context.folder, 0, nullptr)) ||
            context.folder == nullptr)
        {
            if (error != nullptr) *error = L"父文件夹 PIDL 解析失败";
            return false;
        }
    }

    // 空白区没有选中项，只需要父文件夹 PIDL。
    if (paths.empty()) return true;

    for (const std::wstring& path : paths)
    {
        PIDLIST_ABSOLUTE absolute = nullptr;
        {
            if (FAILED(::SHParseDisplayName(path.c_str(), nullptr, &absolute, 0, nullptr)) ||
                absolute == nullptr)
            {
                if (error != nullptr) *error = L"路径 PIDL 解析失败：" + path;
                return false;
            }
        }

        ComPtr<IShellFolder> parent;
        PCUITEMID_CHILD child = nullptr;
        {
            if (FAILED(::SHBindToParent(absolute, IID_PPV_ARGS(parent.GetAddressOf()), &child)) ||
                child == nullptr)
            {
                ::CoTaskMemFree(absolute);
                if (error != nullptr) *error = L"取父文件夹失败：" + path;
                return false;
            }
        }

        context.childPidls.push_back(absolute);
        context.children.push_back(child);
    }

    IDataObject* raw = nullptr;
    const HRESULT hr = ::SHCreateDataObject(context.folder,
                                            static_cast<UINT>(context.children.size()),
                                            context.children.data(),
                                            nullptr,
                                            IID_IDataObject,
                                            reinterpret_cast<void**>(&raw));
    if (SUCCEEDED(hr) && raw != nullptr)
    {
        context.dataObject.Attach(raw);
        return true;
    }

    // 兜底：至少给 CF_HDROP，避免扩展连文件列表都拿不到。
    context.dataObject.Attach(new FileDataObject(paths));
    return true;
}

// 初始化一个经典 shell 扩展；调用方负责在 context 生命周期内使用它。
HRESULT InitializeShellExt(IUnknown* unknown,
                           const std::vector<std::wstring>& paths,
                           const std::wstring& parentFolder,
                           ShellExtContext& context,
                           std::wstring* error)
{
    if (unknown == nullptr)
    {
        if (error != nullptr) *error = L"handler 为空";
        return E_POINTER;
    }

    ComPtr<IShellExtInit> init;
    if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(init.GetAddressOf()))) || init == nullptr)
    {
        if (error != nullptr) *error = L"没有 IShellExtInit";
        return E_NOINTERFACE;
    }

    if (!BuildShellExtContext(paths, parentFolder, context, error)) return E_FAIL;

    return init->Initialize(context.folder, context.dataObject.Get(), nullptr);
}

// ---------------------------------------------------------------------------
// handler 枚举
// ---------------------------------------------------------------------------

struct HandlerRef
{
    std::wstring clsid;
    std::wstring registryPath;
};

void AppendHandlersFromKey(HKEY root, const std::wstring& keyPath,
                           std::vector<HandlerRef>& out,
                           std::set<std::wstring>& seen)
{
    for (const std::wstring& name : RegSubKeys(root, keyPath))
    {
        const std::wstring clsid = RegGetString(root, keyPath + L"\\" + name, nullptr);
        if (clsid.empty()) continue;

        const std::wstring lower = ToLower(clsid);
        if (!seen.insert(lower).second) continue;
        out.push_back({ clsid, keyPath + L"\\" + name });
    }
}

std::vector<HandlerRef> EnumerateFileHandlers(const std::wstring& ext)
{
    std::vector<HandlerRef> refs;
    std::set<std::wstring> seen;

    const std::wstring extLower = ToLower(ext);
    AppendHandlersFromKey(HKEY_CLASSES_ROOT, L"*\\shellex\\ContextMenuHandlers", refs, seen);
    AppendHandlersFromKey(HKEY_CLASSES_ROOT, L"AllFilesystemObjects\\shellex\\ContextMenuHandlers", refs, seen);
    AppendHandlersFromKey(HKEY_CLASSES_ROOT, extLower + L"\\shellex\\ContextMenuHandlers", refs, seen);
    AppendHandlersFromKey(HKEY_CLASSES_ROOT, L"SystemFileAssociations\\" + extLower +
                          L"\\shellex\\ContextMenuHandlers", refs, seen);
    AppendHandlersFromKey(HKEY_CLASSES_ROOT, L"SystemFileAssociations\\image\\shellex\\ContextMenuHandlers", refs, seen);

    const std::wstring progId = RegGetString(HKEY_CLASSES_ROOT, extLower, nullptr);
    if (!progId.empty())
    {
        AppendHandlersFromKey(HKEY_CLASSES_ROOT, progId + L"\\shellex\\ContextMenuHandlers", refs, seen);
    }

    return refs;
}

std::vector<HandlerRef> EnumerateSceneHandlers(unsigned int scene)
{
    std::vector<HandlerRef> refs;
    std::set<std::wstring> seen;

    switch (scene)
    {
    case SceneFolder:
        AppendHandlersFromKey(HKEY_CLASSES_ROOT, L"Directory\\shellex\\ContextMenuHandlers", refs, seen);
        AppendHandlersFromKey(HKEY_CLASSES_ROOT, L"Folder\\shellex\\ContextMenuHandlers", refs, seen);
        AppendHandlersFromKey(HKEY_CLASSES_ROOT, L"AllFilesystemObjects\\shellex\\ContextMenuHandlers", refs, seen);
        break;

    case SceneBackground:
        AppendHandlersFromKey(HKEY_CLASSES_ROOT, L"Directory\\Background\\shellex\\ContextMenuHandlers", refs, seen);
        break;

    case SceneMultiple:
        AppendHandlersFromKey(HKEY_CLASSES_ROOT, L"*\\shellex\\ContextMenuHandlers", refs, seen);
        AppendHandlersFromKey(HKEY_CLASSES_ROOT, L"AllFilesystemObjects\\shellex\\ContextMenuHandlers", refs, seen);
        AppendHandlersFromKey(HKEY_CLASSES_ROOT, L"Directory\\shellex\\ContextMenuHandlers", refs, seen);
        AppendHandlersFromKey(HKEY_CLASSES_ROOT, L"Folder\\shellex\\ContextMenuHandlers", refs, seen);
        break;

    default:
        break;
    }

    return refs;
}

// ---------------------------------------------------------------------------
// 逐 handler 采集
// ---------------------------------------------------------------------------

struct HandlerCapture
{
    std::wstring clsid;
    std::wstring server;
    std::wstring appName;
    std::vector<MenuInventoryItem> items;
    bool ok = false;
    std::wstring failReason;
};

void ApplyScope(std::vector<MenuInventoryItem>& items, unsigned int scenes,
                const std::wstring& ext)
{
    for (MenuInventoryItem& item : items)
    {
        item.scenes = scenes;
        if (!ext.empty()) item.exts.assign(1, ext);
        ApplyScope(item.children, scenes, ext);
    }
}

// ---------------------------------------------------------------------------
// WPS 菜单图标适配
//
// WPS 的 kwpsshellext64.dll 只实现 IContextMenu，QueryContextMenu 返回的
// HMENU 里没有 hbmpItem / owner-draw；但 WPS 自己在 addons\kapplist\shellext\
// menu_2.xml 里给每一项配置了 icon 名字，原生菜单就是用这套资源画的。
// 这里按菜单文字去读 WPS 自己的配置和图标，属于“按 owner 的菜单清单取图标”，
// 不是针对某个菜单项写死补丁；以后 WPS 增删项也能跟着走。
// ---------------------------------------------------------------------------

bool ReadTextFile(const std::wstring& path, std::wstring& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string bytes((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    out = FromUtf8(bytes);
    return !out.empty();
}

std::wstring XmlAttribute(const std::wstring& tag, const wchar_t* name)
{
    const std::wstring doubleQuote = std::wstring(L" ") + name + L"=\"";
    size_t pos = tag.find(doubleQuote);
    if (pos != std::wstring::npos)
    {
        pos += doubleQuote.size();
        const size_t end = tag.find(L'"', pos);
        if (end != std::wstring::npos) return tag.substr(pos, end - pos);
    }

    const std::wstring singleQuote = std::wstring(L" ") + name + L"='";
    pos = tag.find(singleQuote);
    if (pos == std::wstring::npos) return std::wstring();
    pos += singleQuote.size();
    const size_t end = tag.find(L'\'', pos);
    return end == std::wstring::npos ? std::wstring() : tag.substr(pos, end - pos);
}

std::wstring XmlTagText(const std::wstring& xml, const wchar_t* tag, size_t start)
{
    const std::wstring open = std::wstring(L"<") + tag;
    const size_t openPos = xml.find(open, start);
    if (openPos == std::wstring::npos) return std::wstring();
    const size_t gt = xml.find(L'>', openPos);
    if (gt == std::wstring::npos) return std::wstring();
    const std::wstring close = std::wstring(L"</") + tag + L">";
    const size_t closePos = xml.find(close, gt);
    if (closePos == std::wstring::npos) return std::wstring();
    return xml.substr(gt + 1, closePos - gt - 1);
}

std::map<std::wstring, std::wstring> ParseWpsTranslations(const std::wstring& xml)
{
    std::map<std::wstring, std::wstring> result;
    size_t pos = 0;
    while (true)
    {
        const size_t message = xml.find(L"<message>", pos);
        if (message == std::wstring::npos) break;
        const size_t messageEnd = xml.find(L"</message>", message);
        if (messageEnd == std::wstring::npos) break;

        const std::wstring source = XmlTagText(xml, L"source", message);
        const std::wstring translation = XmlTagText(xml, L"translation", message);
        if (!source.empty() && !translation.empty()) result[source] = translation;

        pos = messageEnd + 10;
    }
    return result;
}

struct WpsIconConfig
{
    std::map<std::wstring, std::vector<std::wstring>> iconsByText;
    std::wstring iconDirectory;          // Win11 菜单资产：*.ico
    std::wstring legacyIconDirectory;    // 传统菜单资产：*.png（个别项只有 PNG）
};

bool ParseWpsMenuIcons(const std::wstring& menuXml,
                       const std::map<std::wstring, std::wstring>& translations,
                       WpsIconConfig& out)
{
    size_t pos = 0;
    while (true)
    {
        const size_t lt = menuXml.find(L'<', pos);
        if (lt == std::wstring::npos) break;
        const size_t gt = menuXml.find(L'>', lt);
        if (gt == std::wstring::npos) break;

        const std::wstring tag = menuXml.substr(lt, gt - lt + 1);
        const std::wstring tagName = tag.substr(1, tag.find_first_of(L" \t\r\n>/") - 1);
        const bool isMenuCommand =
            tagName == L"triggerCmd" || tagName == L"listCmd" || tagName == L"aiListCmd" ||
            tagName == L"triggerCmdPdfEdit" || tagName == L"triggerCmdPdfBatchPrint" ||
            tagName == L"triggerCmdPdfBatchRename" || tagName == L"triggerCmdPhotoPlugin" ||
            tagName == L"triggerCmdPhotoBatchPrint" || tagName == L"triggerCmdCDiskClean" ||
            tagName == L"triggerCmdNewPhotoEdit";

        if (isMenuCommand)
        {
            const std::wstring text = XmlAttribute(tag, L"text");
            const std::wstring icon = XmlAttribute(tag, L"icon");
            if (!text.empty() && !icon.empty())
            {
                std::wstring label = text;
                if (!label.empty() && label.front() == L'@')
                {
                    const std::wstring key = label.substr(1);
                    const auto found = translations.find(key);
                    if (found != translations.end()) label = found->second;
                }
                if (!label.empty()) out.iconsByText[label].push_back(icon);
            }
        }
        pos = gt + 1;
    }
    return !out.iconsByText.empty();
}

std::wstring WpsIconPath(const WpsIconConfig& config, const std::wstring& iconName)
{
    if (iconName.empty()) return std::wstring();

    // WPS 传统菜单的资产本来就是 PNG（32×32、带 alpha）；优先用它。
    // winappicon 下的 ICO 是多尺寸帧，个别 ICO 的第一个 16×16 帧只有 4/8 位色，
    // LoadImage 选到那一帧就会把透明区域画成黑底（例如 wpsai.ico）。
    const std::wstring pngPath = config.legacyIconDirectory + L"\\" + iconName + L".png";
    const DWORD pngAttrs = ::GetFileAttributesW(pngPath.c_str());
    if (pngAttrs != INVALID_FILE_ATTRIBUTES && (pngAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return pngPath;
    }

    // 没有 PNG 时才退回 ICO。
    const std::wstring icoPath = config.iconDirectory + L"\\" + iconName + L".ico";
    const DWORD icoAttrs = ::GetFileAttributesW(icoPath.c_str());
    if (icoAttrs != INVALID_FILE_ATTRIBUTES && (icoAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return icoPath;
    }

    return std::wstring();
}

bool IsImageExtension(const std::wstring& ext)
{
    const std::wstring lower = ToLower(ext);
    return lower == L".png" || lower == L".jpg" || lower == L".jpeg" ||
           lower == L".bmp" || lower == L".gif" || lower == L".tif" ||
           lower == L".tiff" || lower == L".webp" || lower == L".ico" ||
           lower == L".psd" || lower == L".svg" || lower == L".heic";
}

std::wstring PickWpsIconName(const std::wstring& label,
                             const std::wstring& extension,
                             const WpsIconConfig& config)
{
    // 菜单文字可能带省略号，配置里的 text 不带；查找前先去掉末尾省略号。
    std::wstring normalized = label;
    while (normalized.size() >= 3 &&
           normalized.compare(normalized.size() - 3, 3, L"...") == 0)
    {
        normalized.erase(normalized.size() - 3);
        while (!normalized.empty() && normalized.back() == L' ') normalized.pop_back();
    }
    if (!normalized.empty() && normalized.back() == L'\x2026')
    {
        normalized.pop_back();
        while (!normalized.empty() && normalized.back() == L' ') normalized.pop_back();
    }

    // WPS 菜单里“转为PDF”同时对应文档的 file2pdf 和图片的 photo2pdf；
    // 按当前样本扩展名选，不能简单取第一个。
    if (normalized == L"转为PDF")
    {
        return IsImageExtension(extension) ? L"photo2pdf" : L"file2pdf";
    }
    if (normalized == L"转为图片")
    {
        return L"pdf2photo";
    }

    // 用户截图里的 WPS 子项是“PPT转PDF / PPT转图片”，配置里没有这两个
    // 固定文字（它们是按文件类型生成的），这里按语义补两个通用规则。
    const std::wstring lower = ToLower(normalized);
    if (lower.find(L"ppt") != std::wstring::npos &&
        lower.find(L"pdf") != std::wstring::npos)
    {
        return L"file2pdf";
    }
    if (lower.find(L"ppt") != std::wstring::npos &&
        (lower.find(L"图片") != std::wstring::npos ||
         lower.find(L"图像") != std::wstring::npos))
    {
        return L"pdf2photo";
    }
    if (normalized == L"PPT转PDF")
    {
        return L"file2pdf";
    }
    if (normalized == L"PPT转图片")
    {
        return L"pdf2photo";
    }
    if (normalized == L"转为视频")
    {
        return L"singlevideoconvert";
    }

    const auto found = config.iconsByText.find(normalized);
    if (found != config.iconsByText.end())
    {
        // 同一文字可能有多个 icon（例如“转为PDF”）；优先返回实际存在的第一个。
        for (const std::wstring& name : found->second)
        {
            if (!WpsIconPath(config, name).empty()) return name;
        }
    }
    return std::wstring();
}

void EnrichWpsMenuIcons(std::vector<MenuInventoryItem>& items,
                        const std::wstring& server,
                        const std::wstring& samplePath)
{
    if (server.empty()) return;
    const std::wstring lowerServer = ToLower(server);
    if (lowerServer.find(L"kwpsshellext") == std::wstring::npos) return;

    const std::wstring officeDir = ParentFolderOf(server);
    const std::wstring addonsDir = officeDir + L"\\addons\\kapplist";
    const std::wstring menuPath = addonsDir + L"\\shellext\\menu_2.xml";
    const std::wstring translationPath = addonsDir + L"\\shellext\\mui\\zh_CN\\tr.xml";

    std::wstring menuXml;
    std::wstring translationXml;
    if (!ReadTextFile(menuPath, menuXml) || !ReadTextFile(translationPath, translationXml))
    {
        return;
    }

    WpsIconConfig config;
    config.iconDirectory = addonsDir + L"\\winappext\\winappicon";
    config.legacyIconDirectory = addonsDir + L"\\shellext\\icon";
    const auto translations = ParseWpsTranslations(translationXml);
    if (!ParseWpsMenuIcons(menuXml, translations, config)) return;

    std::wstring extension;
    if (!samplePath.empty())
    {
        const size_t dot = samplePath.find_last_of(L'.');
        if (dot != std::wstring::npos) extension = samplePath.substr(dot);
    }

    // 递归处理子菜单；只填空位，不覆盖已经采集到的真实 hbmp / owner-draw 图标。
    int filled = 0;
    std::function<void(std::vector<MenuInventoryItem>&)> visit =
        [&](std::vector<MenuInventoryItem>& list)
    {
        for (MenuInventoryItem& item : list)
        {
            if (!item.separator && item.iconFile.empty() && !item.text.empty())
            {
                const std::wstring label = CleanMenuTitle(item.text);
                const std::wstring iconName = PickWpsIconName(label, extension, config);
                const std::wstring iconPath = WpsIconPath(config, iconName);
                if (!iconPath.empty())
                {
                    item.iconFile = iconPath;
                    item.iconSource = L"menu";
                    ++filled;
                    if (item.text == L"WPS AI" || item.text == L"文档合并/拆分")
                    {
                        Logger::Info(L"WPS 父项检查：" + item.text + L"，子项=" +
                                     std::to_wstring(item.children.size()) + L"，图标=" + iconPath);
                    }
                }
            }
            if (!item.children.empty()) visit(item.children);
        }
    };
    visit(items);
    Logger::Info(L"WPS 菜单图标适配：匹配 " + std::to_wstring(filled) +
                 L" 项（图标目录 " + config.iconDirectory + L"）");
}

// 收集一棵 HMENU 下的文字/verb/offset/子菜单。
void CaptureMenuTree(HMENU menu, IContextMenu* cmBase,
                     IContextMenu3* cm3, IContextMenu2* cm2,
                     const MenuOwner& owner,
                     const MenuInvoke& baseInvoke, int level,
                     const std::wstring& parentPath,
                     std::vector<MenuInventoryItem>& out)
{
    if (menu == nullptr) return;

    const int count = ::GetMenuItemCount(menu);
    for (int index = 0; index < count; ++index)
    {
        wchar_t textBuffer[1024] = {};
        MENUITEMINFOW info = {};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_STRING | MIIM_ID | MIIM_SUBMENU | MIIM_FTYPE | MIIM_STATE | MIIM_BITMAP | MIIM_DATA;
        info.dwTypeData = textBuffer;
        info.cch = static_cast<UINT>(std::size(textBuffer));

        if (!::GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &info)) continue;

        MenuInventoryItem item;
        item.level = level;
        item.text = TrimAmpersand(textBuffer);
        item.separator = (info.fType & MFT_SEPARATOR) != 0;
        item.disabled = (info.fState & MFS_DISABLED) != 0;
        item.owner = owner;
        item.invoke = baseInvoke;

        if (info.wID != 0 && baseInvoke.commandOffset >= 0)
        {
            // wID 是宿主分配的命令 ID；用 offset 更稳定。
            item.invoke.commandOffset = static_cast<int>(info.wID) - baseInvoke.commandOffset;
        }

        // GetCommandString 是基础 IContextMenu 接口方法，直接在基接口上调；
        // IContextMenu2/3 只用于 HandleMenuMsg，不能作为取动词名的前提。
        IContextMenu* commandStringOwner = cmBase;
        if (commandStringOwner != nullptr && !item.separator)
        {
            wchar_t verb[512] = {};
            const HRESULT hrVerb = commandStringOwner->GetCommandString(
                    static_cast<UINT_PTR>(item.invoke.commandOffset), GCS_VERBW, nullptr,
                    reinterpret_cast<char*>(verb),
                    static_cast<UINT>(std::size(verb)));
            if (SUCCEEDED(hrVerb) && verb[0] != L'\0')
            {
                item.verb = verb;
                // 执行链路读的是 invoke.verb，这里一并同步，否则执行端拿不到规范动词名、只能回退偏移。
                item.invoke.verb = verb;
            }
        }

        item.id = owner.key + L"|" + parentPath + L"|" +
                  (item.verb.empty() ? item.text : item.verb) + L"|" +
                  std::to_wstring(item.invoke.commandOffset);

        // hbmpItem 只在少数扩展给；WPS 的 logo 是 owner-draw，这里先标 hbmp。
        if (info.hbmpItem != nullptr && info.hbmpItem != HBMMENU_CALLBACK)
        {
            item.iconSource = L"hbmp";
        }
        else
        {
            item.iconSource = L"none";
        }

        // 动态组展开时把每一项的真实图标截下来（hbmp / owner-draw）。
        if (!item.separator)
        {
            item.iconFile = CaptureMenuItemIcon(menu, info, cm3, cm2, item.id);
            if (!item.iconFile.empty()) item.iconSource = L"menu";
        }

        if (info.hSubMenu != nullptr)
        {
            item.children.clear();
            CaptureMenuTree(info.hSubMenu, cmBase, cm3, cm2, owner, baseInvoke, level + 1,
                            parentPath + item.text + L"\x1f", item.children);
        }

        out.push_back(std::move(item));
    }
}

// 让扩展初始化自己的菜单项（WPS 的“更多”子项在这里补齐）。
// 不能广播 WM_INITMENUPOPUP：那个消息只发给窗口，扩展需要的是 IContextMenu2
// 的 HandleMenuMsg，二者走的是完全不同的路径。
// 优先用 IContextMenu3::HandleMenuMsg2：它同时能处理 owner-draw 子菜单。
void InitMenuRecursive(HMENU menu, IContextMenu2* contextMenu2, IContextMenu3* contextMenu3)
{
    if (menu == nullptr || (contextMenu2 == nullptr && contextMenu3 == nullptr)) return;

    LRESULT result = 0;
    bool handled = false;
    if (contextMenu3 != nullptr)
    {
        handled = SUCCEEDED(contextMenu3->HandleMenuMsg2(
            WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(menu), 0, &result));
    }
    if (!handled && contextMenu2 != nullptr)
    {
        contextMenu2->HandleMenuMsg(WM_INITMENUPOPUP,
                                    reinterpret_cast<WPARAM>(menu), 0);
    }

    const int count = ::GetMenuItemCount(menu);
    for (int index = 0; index < count; ++index)
    {
        MENUITEMINFOW info = {};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_SUBMENU;
        if (::GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &info) &&
            info.hSubMenu != nullptr)
        {
            InitMenuRecursive(info.hSubMenu, contextMenu2, contextMenu3);
        }
    }
}

bool CaptureHandlerWithMenu(const std::vector<std::wstring>& paths,
                            const std::wstring& parentFolder,
                            const HandlerRef& ref,
                            unsigned int scene,
                            HandlerCapture& out)
{
    out.clsid = ref.clsid;
    out.server = ClsidServerPath(ref.clsid);
    out.appName = BestAppName(out.server, L"未知应用");

    CLSID clsid = {};
    if (FAILED(::CLSIDFromString(ref.clsid.c_str(), &clsid)))
    {
        out.failReason = L"CLSID 格式错误";
        return false;
    }

    ComPtr<IUnknown> unknown;
    {
        HRESULT hr = ::CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_PPV_ARGS(unknown.GetAddressOf()));
        if (FAILED(hr) || unknown == nullptr)
        {
            out.failReason = L"CoCreateInstance 失败";
            return false;
        }
    }

    ComPtr<IContextMenu> contextMenu;
    HRESULT hr = unknown.As(&contextMenu);
    if (FAILED(hr) || contextMenu == nullptr)
    {
        out.failReason = L"没有 IContextMenu";
        return false;
    }

    // 关键：父文件夹 PIDL + SHCreateDataObject，不要再用 nullptr + 单路径 CF_HDROP。
    // TortoiseSVN 就是靠这个拿到工作副本上下文，才会出 SVN 更新 / 提交等项。
    ShellExtContext context;
    std::wstring initError;
    {
        hr = InitializeShellExt(unknown.Get(), paths, parentFolder, context, &initError);
    }
    if (FAILED(hr))
    {
        out.failReason = initError.empty() ? L"Initialize 失败" : initError;
        return false;
    }

    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr)
    {
        out.failReason = L"CreatePopupMenu 失败";
        return false;
    }

    const int idFirst = 1;
    const int idLast = 0x7FFF;
    {
        hr = contextMenu->QueryContextMenu(menu, 0, idFirst, idLast, CMF_NORMAL);
    }
    if (FAILED(hr))
    {
        ::DestroyMenu(menu);
        out.failReason = L"QueryContextMenu 失败";
        return false;
    }

    // 让扩展初始化自己的子菜单（WPS 的“更多”子项也在这里补齐）。
    // IContextMenu3 优先：Tortoise 这类扩展会在子菜单弹出时再决定内容。
    ComPtr<IContextMenu2> contextMenu2;
    if (FAILED(unknown.As(&contextMenu2))) contextMenu2 = nullptr;

    ComPtr<IContextMenu3> contextMenu3;
    if (FAILED(unknown.As(&contextMenu3))) contextMenu3 = nullptr;

    {
        InitMenuRecursive(menu, contextMenu2.Get(), contextMenu3.Get());
    }

    MenuOwner owner;
    owner.kind = MenuInvokeKind::ShellHandler;
    owner.key = L"shellex:" + ToLower(ref.clsid);
    owner.clsid = ref.clsid;
    owner.server = out.server;
    owner.appName = out.appName;
    owner.displayName = out.appName;

    MenuInvoke invoke;
    invoke.kind = MenuInvokeKind::ShellHandler;
    invoke.clsid = ref.clsid;
    invoke.commandOffset = idFirst;

    {
        CaptureMenuTree(menu, contextMenu.Get(), contextMenu3.Get(), contextMenu2.Get(), owner, invoke, 0, L"", out.items);
    }

    ::DestroyMenu(menu);

    // WPS 的 IContextMenu 不给 hbmpItem，但它在自己的 menu_2.xml 里配置了
    // 每一项的 icon；用 WPS 自己的图标补上，避免所有项都回退成同一个 logo。
    {
        EnrichWpsMenuIcons(out.items, out.server,
                           paths.empty() ? std::wstring() : paths.front());
    }

    for (MenuInventoryItem& item : out.items)
    {
        item.scenes = scene;
    }

    if (out.items.empty())
    {
        out.failReason = L"没有产出菜单项";
        return false;
    }

    out.ok = true;
    return true;
}

// ---------------------------------------------------------------------------
// 静态 verb 采集
// ---------------------------------------------------------------------------

struct StaticRoot
{
    HKEY root = HKEY_CLASSES_ROOT;
    std::wstring path;
};

std::vector<StaticRoot> StaticVerbRoots(const std::wstring& ext)
{
    const std::wstring extLower = ToLower(ext);
    std::vector<StaticRoot> roots;

    roots.push_back({ HKEY_CLASSES_ROOT, L"*\\shell" });
    roots.push_back({ HKEY_CLASSES_ROOT, L"AllFilesystemObjects\\shell" });
    roots.push_back({ HKEY_CLASSES_ROOT, extLower + L"\\shell" });
    roots.push_back({ HKEY_CLASSES_ROOT, L"SystemFileAssociations\\" + extLower + L"\\shell" });
    roots.push_back({ HKEY_CLASSES_ROOT, L"SystemFileAssociations\\image\\shell" });

    const std::wstring progId = RegGetString(HKEY_CLASSES_ROOT, extLower, nullptr);
    if (!progId.empty()) roots.push_back({ HKEY_CLASSES_ROOT, progId + L"\\shell" });

    return roots;
}

std::vector<StaticRoot> StaticVerbRootsForScene(unsigned int scene)
{
    std::vector<StaticRoot> roots;
    switch (scene)
    {
    case SceneFolder:
        roots.push_back({ HKEY_CLASSES_ROOT, L"Directory\\shell" });
        roots.push_back({ HKEY_CLASSES_ROOT, L"Folder\\shell" });
        roots.push_back({ HKEY_CLASSES_ROOT, L"AllFilesystemObjects\\shell" });
        break;

    case SceneBackground:
        roots.push_back({ HKEY_CLASSES_ROOT, L"Directory\\Background\\shell" });
        break;

    case SceneMultiple:
        roots.push_back({ HKEY_CLASSES_ROOT, L"*\\shell" });
        roots.push_back({ HKEY_CLASSES_ROOT, L"AllFilesystemObjects\\shell" });
        roots.push_back({ HKEY_CLASSES_ROOT, L"Directory\\shell" });
        roots.push_back({ HKEY_CLASSES_ROOT, L"Folder\\shell" });
        break;

    default:
        break;
    }
    return roots;
}

void CaptureStaticVerbsFromRoots(const std::vector<StaticRoot>& roots,
                                 unsigned int scene,
                                 const std::wstring& ext,
                                 std::vector<MenuInventoryItem>& out,
                                 std::set<std::wstring>& seen)
{
    for (const StaticRoot& root : roots)
    {
        for (const std::wstring& verb : RegSubKeys(root.root, root.path))
        {
            const std::wstring keyPath = root.path + L"\\" + verb;

            // 明确要求隐藏的 verb 不进配置。
            if (RegHasValue(root.root, keyPath, L"LegacyDisable")) continue;

            std::wstring text = RegGetString(root.root, keyPath, L"MUIVerb");
            if (text.empty()) text = RegGetString(root.root, keyPath, nullptr);
            text = DisplayTextFromRegistry(text, verb);

            const std::wstring commandLine = RegGetString(root.root, keyPath + L"\\command", nullptr);
            const std::wstring delegateExecute = RegGetString(root.root, keyPath, L"DelegateExecute");
            if (commandLine.empty() && delegateExecute.empty()) continue;
            if (text.empty()) continue;

            const std::wstring dedupKey = ToLower(verb) + L"|" + ToLower(commandLine) + L"|" + ToLower(delegateExecute);
            if (!seen.insert(dedupKey).second) continue;

            const std::wstring exe = ExpandEnvironment(ExecutableFromCommand(commandLine));
            std::wstring appName = BestAppName(exe, L"Windows");
            if (delegateExecute.empty() && commandLine.empty()) appName = L"Windows";

            MenuInventoryItem item;
            item.text = text;
            item.verb = verb;
            item.separator = false;
            item.disabled = RegHasValue(root.root, keyPath, L"Extended");
            item.owner.kind = MenuInvokeKind::StaticVerb;
            item.owner.key = L"static:" + ToLower(root.path) + L"\\" + ToLower(verb);
            item.owner.server = exe;
            item.owner.appName = appName;
            item.owner.displayName = appName;
            item.invoke.kind = MenuInvokeKind::StaticVerb;
            item.invoke.verb = verb;
            item.invoke.commandLine = commandLine;
            item.invoke.delegateExecute = delegateExecute;
            item.scenes = scene;
            if (scene == SceneFile && !ext.empty()) item.exts.push_back(ext);
            item.id = item.owner.key + L"|" + ToLower(text);

            out.push_back(std::move(item));
        }
    }
}

void CaptureStaticVerbs(const std::wstring& ext, std::vector<MenuInventoryItem>& out,
                        std::set<std::wstring>& seen)
{
    CaptureStaticVerbsFromRoots(StaticVerbRoots(ext), SceneFile, ext, out, seen);
}

// ---------------------------------------------------------------------------
// Win11 现代右键菜单（IExplorerCommand / PackagedCom）
//
// WinRAR 7.20 默认 LegacyMenu=0、NewContextMenu=1：它不再注册经典
// ContextMenuHandlers，而是通过 sparse package 的 windows.fileExplorerContextMenus
// 发布 IExplorerCommand。只采经典 handler 会漏掉整棵 WinRAR 菜单，所以这里
// 从 PackagedCom 包清单里读 ItemType/Verb，再单独实例化 IExplorerCommand。
// ---------------------------------------------------------------------------

struct PackagedRegistration
{
    std::wstring packageName;
    std::wstring itemType;     // * / .rar / Directory / Directory\Background
    std::wstring clsid;
    std::wstring server;
    std::wstring appName;
};

bool ReadUtf8File(const std::wstring& path, std::wstring& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF)
    {
        bytes.erase(0, 3);
    }
    out = FromUtf8(bytes);
    return true;
}

// 从 XML 开标签里取属性值；name 传小写，例如 L"type" / L"clsid"。
std::wstring XmlAttr(const std::wstring& tag, const std::wstring& name)
{
    const std::wstring lower = ToLower(tag);
    for (wchar_t quote : { L'"', L'\'' })
    {
        const std::wstring needle = L" " + name + L"=" + quote;
        const size_t pos = lower.find(needle);
        if (pos == std::wstring::npos) continue;
        const size_t begin = pos + needle.size();
        const size_t end = tag.find(quote, begin);
        if (end != std::wstring::npos) return tag.substr(begin, end - begin);
    }
    return std::wstring();
}

std::wstring XmlTagText(const std::wstring& xml, const std::wstring& tag)
{
    const std::wstring open = L"<" + tag + L">";
    const std::wstring close = L"</" + tag + L">";
    const size_t begin = xml.find(open);
    if (begin == std::wstring::npos) return std::wstring();
    const size_t end = xml.find(close, begin + open.size());
    if (end == std::wstring::npos) return std::wstring();
    return xml.substr(begin + open.size(), end - begin - open.size());
}

std::vector<PackagedRegistration> LoadPackagedRegistrations()
{
    static std::vector<PackagedRegistration> registrations;
    static bool loaded = false;
    if (loaded) return registrations;
    loaded = true;

    wchar_t programFiles[MAX_PATH] = {};
    if (::GetEnvironmentVariableW(L"ProgramW6432", programFiles, MAX_PATH) == 0)
    {
        ::GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH);
    }
    const std::wstring windowsApps =
        (programFiles[0] == L'\0' ? std::wstring(L"C:\\Program Files") : std::wstring(programFiles)) +
        L"\\WindowsApps\\";

    std::set<std::wstring> seen;
    for (const std::wstring& package : RegSubKeys(HKEY_CLASSES_ROOT, L"PackagedCom\\Package"))
    {
        if (package == L"MaxInstallOrder") continue;

        const std::wstring packageDir = windowsApps + package;
        const std::wstring manifestPath = packageDir + L"\\AppxManifest.xml";
        std::wstring manifest;
        if (!ReadUtf8File(manifestPath, manifest)) continue;
        if (manifest.find(L"windows.fileExplorerContextMenus") == std::wstring::npos) continue;

        std::wstring displayName = XmlTagText(manifest, L"DisplayName");
        if (displayName.empty()) displayName = package;

        const std::wstring lower = ToLower(manifest);
        size_t pos = 0;
        while ((pos = lower.find(L"<desktop5:itemtype", pos)) != std::wstring::npos)
        {
            const size_t openEnd = lower.find(L'>', pos);
            if (openEnd == std::wstring::npos) break;
            const std::wstring openTag = manifest.substr(pos, openEnd - pos + 1);
            const std::wstring itemType = XmlAttr(openTag, L"type");

            const size_t bodyEnd = lower.find(L"</desktop5:itemtype>", openEnd);
            if (bodyEnd == std::wstring::npos) break;
            const std::wstring body = manifest.substr(openEnd + 1, bodyEnd - openEnd - 1);

            size_t verbPos = 0;
            while ((verbPos = ToLower(body).find(L"<desktop5:verb", verbPos)) != std::wstring::npos)
            {
                const size_t verbOpenEnd = body.find(L'>', verbPos);
                if (verbOpenEnd == std::wstring::npos) break;
                const std::wstring verbTag = body.substr(verbPos, verbOpenEnd - verbPos + 1);
                const std::wstring clsid = XmlAttr(verbTag, L"clsid");
                if (!clsid.empty())
                {
                    // CLSIDFromString 在这里只接受带花括号的形式；Appx 清单里
                    // 经常写的是不带花括号的 GUID，所以入库前统一补上。
                    std::wstring clsidText = clsid;
                    if (clsidText.front() != L'{') clsidText = L"{" + clsidText + L"}";

                    const std::wstring key = ToLower(clsidText) + L"|" + ToLower(itemType);
                    if (seen.insert(key).second)
                    {
                        std::wstring server = ClsidServerPath(clsidText);
                        if (server.empty())
                        {
                            std::wstring dllPath = RegGetString(
                                HKEY_CLASSES_ROOT,
                                L"PackagedCom\\Package\\" + package + L"\\Class\\" + clsidText,
                                L"DllPath");
                            if (!dllPath.empty()) server = packageDir + L"\\" + dllPath;
                        }
                        PackagedRegistration reg;
                        reg.packageName = package;
                        reg.itemType = itemType;
                        reg.clsid = clsidText;
                        reg.server = server;
                        reg.appName = BestAppName(server, displayName);
                        registrations.push_back(std::move(reg));
                    }
                }
                verbPos = verbOpenEnd + 1;
            }
            pos = bodyEnd + std::wstring(L"</desktop5:itemtype>").size();
        }
    }

    return registrations;
}

bool PackagedAppliesToExtension(const std::wstring& itemType, const std::wstring& ext)
{
    const std::wstring type = ToLower(itemType);
    const std::wstring extension = ToLower(ext);
    return type == L"*" || type == L"allfilesystemobjects" || type == extension;
}


bool MakeShellItemArray(const std::vector<std::wstring>& paths,
                        const std::wstring& parentFolder,
                        ComPtr<IShellItemArray>& items)
{
    if (paths.empty())
    {
        return false;
    }

    // 关键：IShellItemArray 必须从标准 IDataObject 构造（带正确的父文件夹上下文）。
    // 若改用 SHCreateShellItemArrayFromIDLists 传裸绝对 PIDL，里面的 IShellItem 父文件夹
    // 会挂在桌面，现代扩展（如 WPS）GetTitle 内部逐个 item 绑定时退化走慢路径，
    // 选中文件越多越慢（282 项实测单命令约 8 秒；FromDataObject 仅 2ms）。
    ShellExtContext context;
    std::wstring err;
    if (!BuildShellExtContext(paths, parentFolder, context, &err))
    {
        return false;
    }

    const HRESULT hr = ::SHCreateShellItemArrayFromDataObject(
        context.dataObject.Get(), IID_PPV_ARGS(items.GetAddressOf()));

    return SUCCEEDED(hr) && items != nullptr;
}

std::wstring ExplorerCommandTitle(IExplorerCommand* command, IShellItemArray* items)
{
    if (command == nullptr) return std::wstring();
    wchar_t* title = nullptr;
    if (FAILED(command->GetTitle(items, &title)) || title == nullptr) return std::wstring();
    std::wstring result = TrimAmpersand(title);
    ::CoTaskMemFree(title);
    return result;
}

bool ExplorerCommandState(IExplorerCommand* command, IShellItemArray* items,
                          bool* hidden, bool* disabled)
{
    *hidden = false;
    *disabled = false;
    if (command == nullptr) return false;

    EXPCMDSTATE state = ECS_ENABLED;
    // 菜单构建阶段 fOkToBeSlow 传 FALSE：要求命令不做重内存/重计算；
    // 若返回 E_PENDING 等失败，下面按默认启用处理，重活留到 Invoke 阶段。
    if (FAILED(command->GetState(items, FALSE, &state))) return true;
    *hidden = (state & ECS_HIDDEN) != 0;
    *disabled = (state & ECS_DISABLED) != 0;
    return true;
}

void CaptureExplorerChildren(IExplorerCommand* parent, IShellItemArray* items,
                             const MenuOwner& owner, const MenuInvoke& invoke,
                             int level, const std::wstring& basePath,
                             std::vector<MenuInventoryItem>& out)
{
    if (parent == nullptr) return;

    ComPtr<IEnumExplorerCommand> enumerator;
    if (FAILED(parent->EnumSubCommands(enumerator.GetAddressOf())) || enumerator == nullptr)
    {
        return;
    }

    int index = 0;
    while (true)
    {
        IExplorerCommand* childRaw = nullptr;
        ULONG fetched = 0;
        if (FAILED(enumerator->Next(1, &childRaw, &fetched)) || fetched != 1 || childRaw == nullptr)
        {
            break;
        }
        ComPtr<IExplorerCommand> child;
        child.Attach(childRaw);

        const std::wstring title = ExplorerCommandTitle(child.Get(), items);
        if (title.empty())
        {
            ++index;
            continue;
        }

        bool hidden = false;
        bool disabled = false;
        ExplorerCommandState(child.Get(), items, &hidden, &disabled);
        if (hidden)
        {
            ++index;
            continue;
        }

        const std::wstring path = basePath.empty()
            ? std::to_wstring(index)
            : basePath + L"/" + std::to_wstring(index);

        MenuInventoryItem item;
        item.id = owner.key + L"|" + path + L"|" + ToLower(title);
        item.text = title;
        item.level = level;
        item.disabled = disabled;
        item.owner = owner;
        item.invoke = invoke;
        item.invoke.verb = path;
        item.invoke.commandOffset = index;
        item.iconSource = L"none";
        item.iconFile = CaptureExplorerCommandIcon(child.Get(), items, item.id);

        CaptureExplorerChildren(child.Get(), items, owner, invoke, level + 1, path, item.children);
        out.push_back(std::move(item));
        ++index;
    }
}

bool CapturePackagedCommand(const std::vector<std::wstring>& paths,
                            const PackagedRegistration& reg,
                            const std::wstring& parentFolder,
                            unsigned int scene,
                            std::vector<MenuInventoryItem>& out)
{
    CLSID clsid = {};
    if (FAILED(::CLSIDFromString(reg.clsid.c_str(), &clsid))) return false;

    ComPtr<IExplorerCommand> command;
    {
        if (FAILED(::CoCreateInstance(clsid, nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(command.GetAddressOf()))) || command == nullptr)
        {
            return false;
        }
    }

    ComPtr<IShellItemArray> items;
    {
        if (!MakeShellItemArray(paths, parentFolder, items)) return false;
    }

    std::wstring title;
    {
        title = ExplorerCommandTitle(command.Get(), items.Get());
    }
    if (title.empty()) return false;

    bool hidden = false;
    bool disabled = false;
    {
        ExplorerCommandState(command.Get(), items.Get(), &hidden, &disabled);
    }
    if (hidden) return false;

    MenuOwner owner;
    owner.kind = MenuInvokeKind::PackagedCommand;
    owner.key = L"packaged:" + ToLower(reg.clsid);
    owner.clsid = reg.clsid;
    owner.server = reg.server;
    owner.appName = reg.appName;
    owner.displayName = reg.appName;

    MenuInvoke invoke;
    invoke.kind = MenuInvokeKind::PackagedCommand;
    invoke.clsid = reg.clsid;
    invoke.packageId = reg.packageName;

    MenuInventoryItem item;
    item.id = owner.key + L"|top|" + ToLower(title);
    item.text = title;
    item.level = 0;
    item.disabled = disabled;
    item.owner = owner;
    item.invoke = invoke;
    item.iconSource = L"none";
    {
        item.iconFile = CaptureExplorerCommandIcon(command.Get(), items.Get(), item.id);
    }
    item.scenes = scene;

    {
        CaptureExplorerChildren(command.Get(), items.Get(), owner, invoke, 1, L"", item.children);
    }
    out.push_back(std::move(item));

    return true;
}

bool PackagedAppliesToScene(const PackagedRegistration& reg,
                            unsigned int scene,
                            const std::wstring& ext)
{
    const std::wstring type = ToLower(reg.itemType);
    switch (scene)
    {
    case SceneFile:
        return PackagedAppliesToExtension(reg.itemType, ext);
    case SceneFolder:
        return type == L"directory" || type == L"allfilesystemobjects";
    case SceneBackground:
        return type == L"directory\\background";
    case SceneMultiple:
        return type == L"*" || type == L"allfilesystemobjects";
    default:
        return false;
    }
}

void CapturePackagedCommands(const std::vector<std::wstring>& paths,
                             const std::wstring& parentFolder,
                             unsigned int scene,
                             const std::wstring& ext,
                             std::vector<MenuInventoryItem>& out)
{
    for (const PackagedRegistration& reg : LoadPackagedRegistrations())
    {
        if (!PackagedAppliesToScene(reg, scene, ext)) continue;

        // 经典和现代两套来源都保留；是否同时显示由配置层决定，
        // 这里不再按品牌（WPS 等）跳过，避免把“来源分离”又变成品牌特判。
        std::vector<MenuInventoryItem> captured;
        if (!CapturePackagedCommand(paths, reg, parentFolder, scene, captured)) continue;

        for (MenuInventoryItem& item : captured)
        {
            item.scenes = scene;
            if (scene == SceneFile && !ext.empty()) item.exts.push_back(ext);
            ApplyScope(item.children, scene, ext);
        }
        out.insert(out.end(), captured.begin(), captured.end());
    }
}

// ---------------------------------------------------------------------------
// 场景采集（文件夹 / 空白区 / 多选）
// ---------------------------------------------------------------------------

bool CollectSceneInventoryInternal(unsigned int scene,
                                   const std::wstring& sampleDir,
                                   std::vector<MenuInventoryItem>& out,
                                   std::wstring* error)
{
    out.clear();
    if (error != nullptr) error->clear();
    if (sampleDir.empty())
    {
        if (error != nullptr) *error = L"样本目录为空";
        return false;
    }

    std::vector<std::wstring> paths;
    std::vector<std::wstring> tempFiles;
    std::wstring tempFolder;
    const std::wstring parentFolder = sampleDir;

    switch (scene)
    {
    case SceneFolder:
    {
        tempFolder = sampleDir + L"\\menu-scene-folder";
        ::SHCreateDirectoryExW(nullptr, tempFolder.c_str(), nullptr);
        paths.push_back(tempFolder);
        break;
    }

    case SceneBackground:
        // 空白区没有选中项，只给父文件夹 PIDL。
        break;

    case SceneMultiple:
    {
        const std::wstring first = sampleDir + L"\\menu-scene-multi-a.txt";
        const std::wstring second = sampleDir + L"\\menu-scene-multi-b.txt";
        for (const std::wstring& path : { first, second })
        {
            HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) ::CloseHandle(file);
            tempFiles.push_back(path);
        }
        paths.push_back(first);
        paths.push_back(second);
        break;
    }

    default:
        if (error != nullptr) *error = L"不支持的场景";
        return false;
    }

    // 先采静态 verb（没有 CLSID，但同样有精确 owner）。
    std::set<std::wstring> staticSeen;
    CaptureStaticVerbsFromRoots(StaticVerbRootsForScene(scene), scene, L"",
                                out, staticSeen);

    // 再逐个经典 handler 采。
    std::vector<HandlerRef> handlers = EnumerateSceneHandlers(scene);
    for (const HandlerRef& ref : handlers)
    {
        HandlerCapture capture;
        if (!CaptureHandlerWithMenu(paths, parentFolder, ref, scene, capture))
        {
            Logger::Info(std::wstring(L"右键清单：场景 ") + std::to_wstring(scene) +
                         L" 跳过 handler " + ref.clsid + L"（" + capture.failReason + L"）");
            continue;
        }
        out.insert(out.end(), capture.items.begin(), capture.items.end());
    }

    // 最后采 Win11 现代菜单（按 ItemType 判断这个场景适不适用）。
    // 空白区没有选中项，但现代命令还是要一个 IShellItemArray，传当前文件夹本身。
    std::vector<std::wstring> packagedPaths = paths;
    if (packagedPaths.empty() && scene == SceneBackground)
    {
        packagedPaths.push_back(parentFolder);
    }
    CapturePackagedCommands(packagedPaths, parentFolder, scene, L"", out);

    ApplyScope(out, scene, L"");

    // 清理临时样本。删不掉也不影响结果，下次重建时会覆盖。
    for (const std::wstring& path : tempFiles) ::DeleteFileW(path.c_str());
    if (!tempFolder.empty()) ::RemoveDirectoryW(tempFolder.c_str());

    if (out.empty())
    {
        if (error != nullptr) *error = L"没有采到任何菜单项";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 二级菜单现场采集
// ---------------------------------------------------------------------------

bool CaptureOwnerMenuInternal(MenuInvokeKind kind,
                              const std::wstring& clsid,
                              const std::vector<std::wstring>& paths,
                              const std::wstring& parentFolder,
                              unsigned int scene,
                              std::vector<MenuInventoryItem>& out,
                              std::wstring* error)
{
    out.clear();
    if (error != nullptr) error->clear();
    if (clsid.empty())
    {
        if (error != nullptr) *error = L"CLSID 为空";
        return false;
    }

    // 查询阶段按扩展名去重挑代表项传给扩展判断是否出菜单项；完整选择在
    // 执行阶段（InvokeShellHandler / InvokePackagedCommand）另行重建，不受这里影响。
    std::vector<std::wstring> queryPaths = SelectQueryRepresentatives(paths, kQuerySelectionLimit);

    if (kind == MenuInvokeKind::ShellHandler)
    {
        HandlerRef ref;
        ref.clsid = clsid;
        ref.registryPath = L"live";

        HandlerCapture capture;
        if (!CaptureHandlerWithMenu(queryPaths, parentFolder, ref, scene, capture))
        {
            if (error != nullptr) *error = capture.failReason;
            return false;
        }

        ApplyScope(capture.items, scene, L"");
        out = std::move(capture.items);
        return true;
    }

    if (kind == MenuInvokeKind::PackagedCommand)
    {
        PackagedRegistration reg;
        reg.clsid = clsid;
        reg.appName = BestAppName(ClsidServerPath(clsid), clsid);

        // 现代命令总是要一个 IShellItemArray；空白区就传当前文件夹本身。
        std::vector<std::wstring> packagedPaths = queryPaths;
        if (packagedPaths.empty() && !parentFolder.empty())
        {
            packagedPaths.push_back(parentFolder);
        }

        if (!CapturePackagedCommand(packagedPaths, reg, parentFolder, scene, out) || out.empty())
        {
            if (error != nullptr) *error = L"现代命令没有产出菜单项";
            return false;
        }
        ApplyScope(out, scene, L"");
        return true;
    }

    if (error != nullptr) *error = L"这个来源不支持二级现场采集";
    return false;
}



const wchar_t* InvokeKindName(MenuInvokeKind kind)
{
    switch (kind)
    {
    case MenuInvokeKind::StaticVerb: return L"StaticVerb";
    case MenuInvokeKind::ShellHandler: return L"ShellHandler";
    case MenuInvokeKind::PackagedCommand: return L"PackagedCommand";
    case MenuInvokeKind::WindowsBuiltin: return L"WindowsBuiltin";
    }
    return L"ShellHandler";
}

MenuInvokeKind InvokeKindFromName(const std::wstring& name)
{
    if (name == L"StaticVerb") return MenuInvokeKind::StaticVerb;
    if (name == L"PackagedCommand") return MenuInvokeKind::PackagedCommand;
    if (name == L"WindowsBuiltin") return MenuInvokeKind::WindowsBuiltin;
    return MenuInvokeKind::ShellHandler;
}

json ItemToJson(const MenuInventoryItem& item)
{
    json j;
    j["id"] = ToUtf8(item.id);
    j["text"] = ToUtf8(item.text);
    j["verb"] = ToUtf8(item.verb);
    j["level"] = item.level;
    j["separator"] = item.separator;
    j["disabled"] = item.disabled;
    j["iconFile"] = ToUtf8(item.iconFile);
    j["iconSource"] = ToUtf8(item.iconSource);
    j["scenes"] = item.scenes;

    j["owner"] = json::object();
    j["owner"]["kind"] = ToUtf8(InvokeKindName(item.owner.kind));
    j["owner"]["key"] = ToUtf8(item.owner.key);
    j["owner"]["clsid"] = ToUtf8(item.owner.clsid);
    j["owner"]["server"] = ToUtf8(item.owner.server);
    j["owner"]["appName"] = ToUtf8(item.owner.appName);
    j["owner"]["displayName"] = ToUtf8(item.owner.displayName);

    j["invoke"] = json::object();
    j["invoke"]["kind"] = ToUtf8(InvokeKindName(item.invoke.kind));
    j["invoke"]["clsid"] = ToUtf8(item.invoke.clsid);
    j["invoke"]["verb"] = ToUtf8(item.invoke.verb);
    j["invoke"]["commandOffset"] = item.invoke.commandOffset;
    j["invoke"]["commandLine"] = ToUtf8(item.invoke.commandLine);
    j["invoke"]["delegateExecute"] = ToUtf8(item.invoke.delegateExecute);
    j["invoke"]["packageId"] = ToUtf8(item.invoke.packageId);

    json exts = json::array();
    for (const std::wstring& ext : item.exts) exts.push_back(ToUtf8(ext));
    j["exts"] = exts;

    json children = json::array();
    for (const MenuInventoryItem& child : item.children) children.push_back(ItemToJson(child));
    j["children"] = children;
    return j;
}

MenuInventoryItem ItemFromJson(const json& j)
{
    MenuInventoryItem item;
    item.id = FromUtf8(j.value("id", std::string()));
    item.text = FromUtf8(j.value("text", std::string()));
    item.verb = FromUtf8(j.value("verb", std::string()));
    item.level = j.value("level", 0);
    item.separator = j.value("separator", false);
    item.disabled = j.value("disabled", false);
    item.iconFile = FromUtf8(j.value("iconFile", std::string()));
    item.iconSource = FromUtf8(j.value("iconSource", std::string()));
    item.scenes = j.value("scenes", 0u);

    if (j.contains("owner") && j["owner"].is_object())
    {
        const json& owner = j["owner"];
        item.owner.kind = InvokeKindFromName(FromUtf8(owner.value("kind", std::string())));
        item.owner.key = FromUtf8(owner.value("key", std::string()));
        item.owner.clsid = FromUtf8(owner.value("clsid", std::string()));
        item.owner.server = FromUtf8(owner.value("server", std::string()));
        item.owner.appName = FromUtf8(owner.value("appName", std::string()));
        item.owner.displayName = FromUtf8(owner.value("displayName", std::string()));
    }

    if (j.contains("invoke") && j["invoke"].is_object())
    {
        const json& invoke = j["invoke"];
        item.invoke.kind = InvokeKindFromName(FromUtf8(invoke.value("kind", std::string())));
        item.invoke.clsid = FromUtf8(invoke.value("clsid", std::string()));
        item.invoke.verb = FromUtf8(invoke.value("verb", std::string()));
        item.invoke.commandOffset = invoke.value("commandOffset", -1);
        item.invoke.commandLine = FromUtf8(invoke.value("commandLine", std::string()));
        item.invoke.delegateExecute = FromUtf8(invoke.value("delegateExecute", std::string()));
        item.invoke.packageId = FromUtf8(invoke.value("packageId", std::string()));
    }

    if (j.contains("exts") && j["exts"].is_array())
    {
        for (const auto& ext : j["exts"]) item.exts.push_back(FromUtf8(ext.get<std::string>()));
    }
    if (j.contains("children") && j["children"].is_array())
    {
        for (const auto& child : j["children"]) item.children.push_back(ItemFromJson(child));
    }
    return item;
}

} // namespace

// ---------------------------------------------------------------------------
// 动态组 owner 缓存
//
// 只枚举「哪个应用 / 哪个 handler 在哪个场景出现」：
//   · 枚举阶段不调用 handler；
//   · 重建缓存时对每个 owner 做一次“标题+健康”探测，只存一级菜单文字和状态；
//   · 不保存扩展名矩阵，也不保存功能树；
//   · 真正展开二级菜单时才 CaptureOwnerMenu。
//
// 这样配置窗口打开时只读一份几十 KB 的 owner 清单，不再吃 48MB 普查。
// ---------------------------------------------------------------------------

std::wstring DynamicOwnerCachePath()
{
    const std::wstring appData = AppDataDir();
    if (appData.empty()) return std::wstring();
    return appData + L"\\FileExplorer\\menu-owners.json";
}

bool SaveDynamicOwnerCache(const std::vector<DynamicOwner>& owners)
{
    const std::wstring path = DynamicOwnerCachePath();
    if (path.empty()) return false;

    SYSTEMTIME t = {};
    ::GetLocalTime(&t);
    wchar_t scannedAt[64] = {};
    swprintf_s(scannedAt, L"%04u-%02u-%02u %02u:%02u:%02u",
               t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);

    json root;
    root["version"] = 3;
    root["scannedAt"] = ToUtf8(scannedAt);

    json list = json::array();
    for (const DynamicOwner& owner : owners)
    {
        json j;
        j["kind"]        = ToUtf8(InvokeKindName(owner.kind));
        j["key"]         = ToUtf8(owner.key);
        j["clsid"]       = ToUtf8(owner.clsid);
        j["server"]      = ToUtf8(owner.server);
        j["app"]         = ToUtf8(owner.appName);
        j["scenes"]      = owner.scenes;
        j["sampleExt"]   = ToUtf8(owner.sampleExt);
        j["displayName"] = ToUtf8(owner.displayName);
        j["nameSource"]  = ToUtf8(owner.nameSource);
        j["probeStatus"] = ToUtf8(owner.probeStatus);
        j["probeError"]  = ToUtf8(owner.probeError);
        j["classicProbe"] = ToUtf8(owner.classicProbe);
        list.push_back(j);
    }
    root["owners"] = list;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const std::string text = root.dump(2);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

bool LoadDynamicOwnerCache(std::vector<DynamicOwner>& owners)
{
    owners.clear();
    const std::wstring path = DynamicOwnerCachePath();
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    json root;
    try { in >> root; }
    catch (const std::exception& e)
    {
        Logger::Warn(std::wstring(L"owner 缓存解析失败：") + FromUtf8(e.what()));
        return false;
    }
    const int version = root.value("version", 0);
    // 2=旧 owner 缓存（只有 app 名）；3=增加 displayName / probeStatus。
    if (version != 2 && version != 3) return false;
    if (!root.contains("owners") || !root["owners"].is_array()) return false;

    for (const auto& j : root["owners"])
    {
        DynamicOwner owner;
        owner.kind        = InvokeKindFromName(FromUtf8(j.value("kind", std::string())));
        owner.key         = FromUtf8(j.value("key", std::string()));
        owner.clsid       = FromUtf8(j.value("clsid", std::string()));
        owner.server      = FromUtf8(j.value("server", std::string()));
        owner.appName     = FromUtf8(j.value("app", std::string()));
        owner.scenes      = j.value("scenes", 0u);
        owner.sampleExt   = FromUtf8(j.value("sampleExt", std::string()));
        owner.displayName = FromUtf8(j.value("displayName", std::string()));
        owner.nameSource  = FromUtf8(j.value("nameSource", std::string()));
        owner.probeStatus = FromUtf8(j.value("probeStatus", std::string()));
        owner.probeError  = FromUtf8(j.value("probeError", std::string()));
        owner.classicProbe = FromUtf8(j.value("classicProbe", std::string()));

        // 旧缓存没有标题字段：先退回应用名，等下次 --build-dynamic-cache 再补真实标题。
        if (owner.displayName.empty()) owner.displayName = owner.appName;
        if (owner.nameSource.empty()) owner.nameSource = L"app";
        if (owner.probeStatus.empty()) owner.probeStatus = L"unknown";
        owners.push_back(std::move(owner));
    }
    return !owners.empty();
}

bool EnumerateDynamicOwners(std::vector<DynamicOwner>& owners, std::wstring* error)
{
    owners.clear();
    if (error != nullptr) error->clear();

    // 注册表里的 handler 键名，例如“Open With”“EPP”“CopyAsPathMenu”。
    // 它不是最终菜单文字，但比厂商名“Windows 系统”更有区分度，探测失败时用它兜底。
    auto handlerKeyName = [](const std::wstring& registryPath) -> std::wstring
    {
        const size_t slash = registryPath.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return registryPath;
        return registryPath.substr(slash + 1);
    };

    // CLSID 的默认注册名（英文技术名，例如“Copy as Path Menu”）。
    auto clsidDefaultName = [](const std::wstring& clsid) -> std::wstring
    {
        if (clsid.empty()) return std::wstring();
        return RegGetString(HKEY_CLASSES_ROOT, L"CLSID\\" + clsid, nullptr);
    };

    // 探测前的回退显示名。优先级：handler 键名 > CLSID 默认名 > 应用名。
    // 绝不再回退成“Windows 系统”，否则又会出现一排同名项。
    auto fallbackName = [&](const std::wstring& registryPath,
                            const std::wstring& clsid,
                            const std::wstring& appName,
                            std::wstring& nameSource) -> std::wstring
    {
        const std::wstring keyName = handlerKeyName(registryPath);
        if (!keyName.empty() && keyName.find(L'{') == std::wstring::npos)
        {
            nameSource = L"registry";
            return keyName;
        }

        const std::wstring clsName = clsidDefaultName(clsid);
        if (!clsName.empty())
        {
            nameSource = L"clsid";
            return clsName;
        }

        nameSource = L"app";
        return appName;
    };

    // key -> 已加入的下标；同一 CLSID 在多个场景出现时只累加 scenes。
    std::map<std::wstring, size_t> index;

    auto addOwner = [&](MenuInvokeKind kind, const std::wstring& clsid,
                        const std::wstring& server, const std::wstring& appName,
                        unsigned int scene, const std::wstring& sampleExt,
                        const std::wstring& fallbackDisplayName,
                        const std::wstring& fallbackSource)
    {
        if (clsid.empty()) return;
        const std::wstring prefix = kind == MenuInvokeKind::ShellHandler
                                        ? L"shellex:" : L"packaged:";
        const std::wstring key = prefix + ToLower(clsid);
        auto found = index.find(key);
        if (found != index.end())
        {
            DynamicOwner& existing = owners[found->second];
            existing.scenes |= scene;
            if (existing.sampleExt.empty()) existing.sampleExt = sampleExt;
            if (existing.displayName.empty())
            {
                existing.displayName = fallbackDisplayName;
                existing.nameSource = fallbackSource;
            }
            return;
        }

        DynamicOwner owner;
        owner.kind        = kind;
        owner.key         = key;
        owner.clsid       = clsid;
        owner.server      = server;
        owner.appName     = appName.empty() ? L"未识别" : appName;
        owner.scenes      = scene;
        owner.sampleExt   = sampleExt;
        owner.displayName = fallbackDisplayName;
        owner.nameSource  = fallbackSource;
        owner.probeStatus = L"unchecked";
        index[key] = owners.size();
        owners.push_back(std::move(owner));
    };

    // 经典 shell handler：文件场景按扩展名登记表扫一遍，只读注册表。
    for (const std::wstring& ext : EnumerateExtensions())
    {
        for (const HandlerRef& ref : EnumerateFileHandlers(ext))
        {
            const std::wstring key = L"shellex:" + ToLower(ref.clsid);
            if (index.find(key) != index.end())
            {
                addOwner(MenuInvokeKind::ShellHandler, ref.clsid, L"", L"", SceneFile, ext,
                         L"", L"");
                continue;
            }
            const std::wstring server = ClsidServerPath(ref.clsid);
            const std::wstring appName = BestAppName(server, L"未识别");
            std::wstring nameSource;
            const std::wstring displayName = fallbackName(ref.registryPath, ref.clsid,
                                                          appName, nameSource);
            addOwner(MenuInvokeKind::ShellHandler, ref.clsid, server, appName, SceneFile, ext,
                     displayName, nameSource);
        }
    }

    // 文件夹 / 空白区 / 多选：同样只扫注册表里的 handler 名单。
    for (unsigned int scene : { SceneFolder, SceneBackground, SceneMultiple })
    {
        for (const HandlerRef& ref : EnumerateSceneHandlers(scene))
        {
            const std::wstring key = L"shellex:" + ToLower(ref.clsid);
            if (index.find(key) != index.end())
            {
                addOwner(MenuInvokeKind::ShellHandler, ref.clsid, L"", L"", scene, L"",
                         L"", L"");
                continue;
            }
            const std::wstring server = ClsidServerPath(ref.clsid);
            const std::wstring appName = BestAppName(server, L"未识别");
            std::wstring nameSource;
            const std::wstring displayName = fallbackName(ref.registryPath, ref.clsid,
                                                          appName, nameSource);
            addOwner(MenuInvokeKind::ShellHandler, ref.clsid, server, appName, scene, L"",
                     displayName, nameSource);
        }
    }

    // Win11 现代菜单：从 AppxManifest 登记表里拿 owner。
    for (const PackagedRegistration& reg : LoadPackagedRegistrations())
    {
        unsigned int scenes = 0;
        std::wstring sampleExt;
        const std::wstring type = ToLower(reg.itemType);
        if (type == L"*" || type == L"allfilesystemobjects")
        {
            scenes = SceneFile | SceneMultiple;
            sampleExt = L".txt";
        }
        else if (type == L"directory")
        {
            scenes = SceneFolder;
        }
        else if (type == L"directory\\background")
        {
            scenes = SceneBackground;
        }
        else if (!type.empty() && type.front() == L'.')
        {
            scenes = SceneFile;
            sampleExt = type;
        }
        else
        {
            scenes = SceneFile;
            sampleExt = L".txt";
        }
        addOwner(MenuInvokeKind::PackagedCommand, reg.clsid, reg.server,
                 reg.appName, scenes, sampleExt, reg.appName, L"app");
    }

    // 按应用名排序，界面树稳定；同名再按 key 排。
    std::sort(owners.begin(), owners.end(),
              [](const DynamicOwner& a, const DynamicOwner& b)
              {
                  const int byApp = _wcsicmp(a.appName.c_str(), b.appName.c_str());
                  if (byApp != 0) return byApp < 0;
                  return _wcsicmp(a.key.c_str(), b.key.c_str()) < 0;
              });

    if (owners.empty())
    {
        if (error != nullptr) *error = L"没有枚举到任何右键 owner";
        return false;
    }
    return true;
}

namespace
{

// 一次 owner 探测用到的样本规格：场景 + 扩展名。
struct OwnerProbeSpec
{
    unsigned int scene = 0;
    std::wstring ext;
};

struct OwnerProbeResult
{
    std::wstring title;      // 抓到的真实一级菜单文字
    std::wstring status;     // ok / unavailable / invalid / unchecked
    std::wstring error;      // 最后一次失败原因
};

// 这些错误说明 handler 本身不可用，不是“当前样本不适用”。
// 只有这一类会在缓存里剔除；Initialize/QueryContextMenu 失败仍保留，
// 让配置界面可以显示“当前环境读取失败”，而不是把正常软件误删。
bool IsHardProbeError(const std::wstring& error)
{
    return error.find(L"CoCreateInstance") != std::wstring::npos ||
           error.find(L"CLSID 格式") != std::wstring::npos ||
           error.find(L"CLSID 为空") != std::wstring::npos ||
           error.find(L"没有 IContextMenu") != std::wstring::npos ||
           error.find(L"没有 IShellExtInit") != std::wstring::npos;
}

std::wstring NormalizeProbeExt(std::wstring ext)
{
    if (ext.empty()) return L".txt";
    if (ext.front() != L'.') ext.insert(ext.begin(), L'.');
    return ext;
}

// 候选扩展名：先用登记时找到的 sampleExt，再用几种常见文件类型兜底。
// 目的是解决 WPS 这类“只在 Office 文件上初始化成功”的 handler；
// 探测只拿菜单标题，不会把兜底扩展名写进配置。
std::vector<std::wstring> ProbeExtensions(const DynamicOwner& owner)
{
    std::vector<std::wstring> exts;
    std::set<std::wstring> seen;

    auto add = [&](const std::wstring& ext)
    {
        const std::wstring normalized = ToLower(NormalizeProbeExt(ext));
        if (seen.insert(normalized).second) exts.push_back(normalized);
    };

    add(owner.sampleExt);
    add(L".docx");
    add(L".xlsx");
    add(L".pptx");
    add(L".txt");
    add(L".pdf");
    add(L".jpg");
    add(L".zip");
    return exts;
}

// 按场景生成探测顺序：文件优先，其次文件夹、空白区、多选。
std::vector<OwnerProbeSpec> BuildProbeSpecs(const DynamicOwner& owner)
{
    std::vector<OwnerProbeSpec> specs;
    const std::vector<std::wstring> exts = ProbeExtensions(owner);

    if ((owner.scenes & SceneFile) != 0)
    {
        for (const std::wstring& ext : exts)
        {
            specs.push_back({ SceneFile, ext });
        }
    }
    if ((owner.scenes & SceneFolder) != 0)
    {
        specs.push_back({ SceneFolder, std::wstring() });
    }
    if ((owner.scenes & SceneBackground) != 0)
    {
        specs.push_back({ SceneBackground, std::wstring() });
    }
    if ((owner.scenes & SceneMultiple) != 0)
    {
        specs.push_back({ SceneMultiple, exts.empty() ? L".txt" : exts.front() });
    }
    return specs;
}

std::wstring CreateProbeRoot()
{
    wchar_t tempPath[MAX_PATH] = {};
    if (::GetTempPathW(MAX_PATH, tempPath) == 0) return std::wstring();

    wchar_t name[96] = {};
    swprintf_s(name, L"FileExplorer-owner-probe-%lu-%llu",
               ::GetCurrentProcessId(), static_cast<unsigned long long>(::GetTickCount64()));
    const std::wstring root = std::wstring(tempPath) + name;
    ::CreateDirectoryW(root.c_str(), nullptr);
    return root;
}

bool CreateProbeFile(const std::wstring& path)
{
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    ::CloseHandle(file);
    return true;
}

// 一级菜单标题：取第一个非分隔符、非空文字。
// “打开方式”“发送到”这类项本身就是子菜单，也仍然是一级文字。
std::wstring FirstMenuTitle(const std::vector<MenuInventoryItem>& items)
{
    for (const MenuInventoryItem& item : items)
    {
        if (item.separator || item.text.empty()) continue;
        if (item.level != 0) continue;
        return CleanMenuTitle(item.text);
    }
    return std::wstring();
}

bool RunOwnerProbe(const DynamicOwner& owner,
                   const OwnerProbeSpec& spec,
                   std::wstring& title,
                   std::wstring& error)
{
    title.clear();
    error.clear();

    const std::wstring root = CreateProbeRoot();
    if (root.empty())
    {
        error = L"创建探测临时目录失败";
        return false;
    }

    std::vector<std::wstring> paths;
    std::vector<std::wstring> files;
    std::vector<std::wstring> folders;

    const std::wstring parentFolder = root;
    switch (spec.scene)
    {
    case SceneFile:
    {
        const std::wstring sample = root + L"\\probe" + NormalizeProbeExt(spec.ext);
        if (!CreateProbeFile(sample))
        {
            error = L"创建探测样本文件失败";
            ::RemoveDirectoryW(root.c_str());
            return false;
        }
        files.push_back(sample);
        paths.push_back(sample);
        break;
    }

    case SceneFolder:
    {
        const std::wstring folder = root + L"\\probe-folder";
        if (!::CreateDirectoryW(folder.c_str(), nullptr))
        {
            error = L"创建探测样本文件夹失败";
            ::RemoveDirectoryW(root.c_str());
            return false;
        }
        folders.push_back(folder);
        paths.push_back(folder);
        break;
    }

    case SceneBackground:
        // 空白区没有选中项，只给父文件夹 PIDL。
        break;

    case SceneMultiple:
    {
        const std::wstring first = root + L"\\probe-multi-a" + NormalizeProbeExt(spec.ext);
        const std::wstring second = root + L"\\probe-multi-b" + NormalizeProbeExt(spec.ext);
        if (!CreateProbeFile(first) || !CreateProbeFile(second))
        {
            error = L"创建多选探测样本失败";
            ::DeleteFileW(first.c_str());
            ::DeleteFileW(second.c_str());
            ::RemoveDirectoryW(root.c_str());
            return false;
        }
        files.push_back(first);
        files.push_back(second);
        paths.push_back(first);
        paths.push_back(second);
        break;
    }

    default:
        error = L"不支持的探测场景";
        ::RemoveDirectoryW(root.c_str());
        return false;
    }

    std::vector<MenuInventoryItem> items;
    const bool ok = CaptureOwnerMenuInternal(owner.kind, owner.clsid, paths,
                                             parentFolder, spec.scene, items, &error);
    if (ok) title = FirstMenuTitle(items);

    // 清理临时样本；删不掉也不影响下一次探测。
    for (const std::wstring& file : files) ::DeleteFileW(file.c_str());
    for (const std::wstring& folder : folders) ::RemoveDirectoryW(folder.c_str());
    ::RemoveDirectoryW(root.c_str());

    if (!ok) return false;
    if (title.empty())
    {
        error = L"菜单没有可用的一级文字";
        return false;
    }
    return true;
}

// 少数 Windows 内置 handler 的一级菜单是动态状态或子项，直接取第一条文字
// 会得到“文件夹”“正在搜索设备...”这种不像标题的结果。这里只对这几个
// 已知 CLSID 做标题修正；第三方来源完全按现场探测结果，不做品牌特判。
std::wstring KnownBuiltinOwnerTitle(const std::wstring& clsid)
{
    const std::wstring lower = ToLower(clsid);
    if (lower == L"{d969a300-e7ff-11d0-a93b-00a0c90f2719}") return L"新建";
    if (lower == L"{7ad84985-87b4-4a16-be58-8b72a5b390f7}") return L"播放到";
    if (lower == L"{f81e9010-6ea4-11ce-a7ff-00aa003ca9f6}") return L"共享";
    if (lower == L"{7444c717-39bf-11d1-8cd9-00c04fc29d45}") return L"证书";
    return std::wstring();
}

// 探测不到一级文字时，给 Windows 自带来源一个固定的中文兜底名。
// 这些名字只用于“显示系统项”时的可读性；状态仍然是 unavailable，
// 悬停提示会明确写“读取失败”，不会被当成可用来源。
std::wstring KnownBuiltinFallbackTitle(const std::wstring& clsid)
{
    const std::wstring lower = ToLower(clsid);
    if (lower == L"{09a47860-11b0-4da5-afa5-26d86198a780}") return L"Defender 扫描";
    if (lower == L"{0af96ede-aebf-41ed-a1c8-cf7a685505b6}") return L"库";
    if (lower == L"{1d27f844-3a1f-4410-85ac-14651078412d}") return L"兼容性疑难解答";
    if (lower == L"{474c98ee-cf3d-41f5-80e3-4aab0ab04301}") return L"脱机文件";
    if (lower == L"{a470f8cf-a1e8-4f65-8335-227475aa5c46}") return L"加密文件";
    if (lower == L"{e61bf828-5e63-4287-bef1-60b1a4fde0e3}") return L"工作文件夹";
    if (lower == L"{ffe2a43c-56b9-4bf5-9a79-cc6d4285608a}") return L"照片查看器";
    return std::wstring();
}

OwnerProbeResult ProbeOwner(const DynamicOwner& owner)
{
    OwnerProbeResult result;
    result.status = L"unchecked";

    const std::vector<OwnerProbeSpec> specs = BuildProbeSpecs(owner);
    if (specs.empty())
    {
        result.error = L"没有可探测的场景";
        return result;
    }

    bool sawSoftFailure = false;
    for (const OwnerProbeSpec& spec : specs)
    {
        std::wstring title;
        std::wstring error;
        if (RunOwnerProbe(owner, spec, title, error))
        {
            const std::wstring knownTitle = KnownBuiltinOwnerTitle(owner.clsid);
            result.title  = knownTitle.empty() ? title : knownTitle;
            result.status = L"ok";
            result.error.clear();
            return result;
        }

        if (IsHardProbeError(error)) continue;
        sawSoftFailure = true;
        result.error = error;

        Logger::Info(L"动态组 owner 探测失败：" + owner.clsid +
                     L"（" + owner.appName + L"，场景 " + std::to_wstring(spec.scene) +
                     L"，扩展名 " + (spec.ext.empty() ? L"无" : spec.ext) +
                     L"）：" + error);
    }

    // 至少有一次是 Initialize/QueryContextMenu 这类软失败，就保留这个来源；
    // 全部都是 CLSID 无效/没有 IContextMenu，才判定为硬无效并从缓存剔除。
    result.status = sawSoftFailure ? L"unavailable" : L"invalid";
    if (result.error.empty()) result.error = L"所有探测样本都失败";
    return result;
}

} // namespace

bool BuildDynamicOwnerCache()
{
    std::vector<DynamicOwner> owners;
    std::wstring error;
    if (!EnumerateDynamicOwners(owners, &error))
    {
        Logger::Warn(L"动态组 owner 缓存生成失败：" + error);
        return false;
    }

    // 只为“名字 + 健康状态”探测一次，不保存菜单项树。
    // 配置窗口打开和真实右键展开仍然不读旧普查、不预先采菜单内容。
    std::vector<DynamicOwner> kept;
    kept.reserve(owners.size());
    int okCount = 0;
    int unavailableCount = 0;
    int invalidCount = 0;

    for (DynamicOwner& owner : owners)
    {
        const OwnerProbeResult probe = ProbeOwner(owner);
        owner.probeStatus = probe.status;
        owner.probeError  = probe.error;

        if (!probe.title.empty())
        {
            owner.displayName = probe.title;
            owner.nameSource  = L"menu";
        }
        else
        {
            const std::wstring builtinName = KnownBuiltinFallbackTitle(owner.clsid);
            if (!builtinName.empty())
            {
                owner.displayName = builtinName;
                owner.nameSource  = L"builtin";
            }
        }

        if (probe.status == L"invalid")
        {
            ++invalidCount;
            Logger::Warn(L"动态组 owner 已剔除（硬无效）：" + owner.clsid +
                         L"（" + owner.appName + L"）：" + owner.probeError);
            continue;
        }

        if (probe.status == L"ok") ++okCount;
        else if (probe.status == L"unavailable") ++unavailableCount;
        kept.push_back(std::move(owner));
    }

    if (kept.empty())
    {
        Logger::Warn(L"动态组 owner 缓存生成失败：所有来源都不可用");
        return false;
    }

    if (!SaveDynamicOwnerCache(kept))
    {
        Logger::Warn(L"动态组 owner 缓存写入失败");
        return false;
    }
    Logger::Info(L"动态组 owner 缓存已生成：" + std::to_wstring(kept.size()) +
                 L" 个来源（可用 " + std::to_wstring(okCount) +
                 L"，读取失败 " + std::to_wstring(unavailableCount) +
                 L"，剔除 " + std::to_wstring(invalidCount) + L"）");
    return true;
}

bool EnsureClassicFallbacks()
{
    std::vector<DynamicOwner> owners;
    if (!LoadDynamicOwnerCache(owners) || owners.empty())
    {
        return false;
    }

    std::set<std::wstring> shellClsids;
    std::set<std::wstring> shellApps;
    for (const DynamicOwner& owner : owners)
    {
        if (owner.kind != MenuInvokeKind::ShellHandler) continue;
        shellClsids.insert(ToLower(owner.clsid));
        if (!owner.appName.empty()) shellApps.insert(ToLower(owner.appName));
    }

    std::vector<DynamicOwner> additions;
    bool changed = false;

    for (DynamicOwner& owner : owners)
    {
        if (owner.kind != MenuInvokeKind::PackagedCommand) continue;
        if (!owner.classicProbe.empty()) continue;

        const std::wstring clsid = ToLower(owner.clsid);
        if (clsid.empty() || shellClsids.count(clsid) != 0 ||
            (!owner.appName.empty() && shellApps.count(ToLower(owner.appName)) != 0))
        {
            owner.classicProbe = L"skip";   // 已经有经典来源，不重复补
            changed = true;
            continue;
        }

        const std::vector<OwnerProbeSpec> specs = BuildProbeSpecs(owner);
        if (specs.empty())
        {
            owner.classicProbe = L"skip";
            changed = true;
            continue;
        }

        // 只试第一个样本（登记时的 sampleExt / 第一个场景），不把每个扩展名都扫一遍。
        DynamicOwner probe = owner;
        probe.kind = MenuInvokeKind::ShellHandler;

        std::wstring title;
        std::wstring error;
        if (!RunOwnerProbe(probe, specs.front(), title, error))
        {
            owner.classicProbe = L"failed";
            changed = true;
            Logger::Info(L"经典能力探测失败：" + owner.clsid + L"（" + owner.appName +
                         L"）：" + error);
            continue;
        }

        DynamicOwner classic = owner;
        classic.kind         = MenuInvokeKind::ShellHandler;
        classic.key          = L"shellex:" + clsid;
        classic.displayName  = title.empty() ? owner.displayName : title;
        classic.nameSource   = L"classic-probe";
        classic.probeStatus  = L"ok";
        classic.probeError.clear();
        classic.classicProbe = L"ok";
        additions.push_back(std::move(classic));

        owner.classicProbe = L"ok";
        shellClsids.insert(clsid);
        if (!owner.appName.empty()) shellApps.insert(ToLower(owner.appName));
        changed = true;

        Logger::Info(L"已补经典菜单来源：" + owner.appName + L"（" + owner.clsid + L"）");
    }

    if (!changed) return false;

    const size_t addedCount = additions.size();
    owners.insert(owners.end(),
                  std::make_move_iterator(additions.begin()),
                  std::make_move_iterator(additions.end()));
    if (!SaveDynamicOwnerCache(owners))
    {
        Logger::Warn(L"经典菜单来源写回 owner 缓存失败");
        return false;
    }
    Logger::Info(L"经典菜单来源已写回 owner 缓存：新增 " +
                 std::to_wstring(addedCount) + L" 条");
    return addedCount > 0;
}

// ---------------------------------------------------------------------------
// 对外：采集
// ---------------------------------------------------------------------------

bool CollectHandlerInventory(const std::wstring& sampleDir,
                             const std::wstring& ext,
                             const std::wstring& clsid,
                             MenuInventoryExtension& out)
{
    out = MenuInventoryExtension();
    out.ext = ext;

    const std::wstring samplePath = sampleDir + L"\\menu-inventory-sample" + ext;
    HANDLE file = ::CreateFileW(samplePath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        out.failReason = L"创建样本文件失败";
        return false;
    }
    ::CloseHandle(file);
    out.samplePath = samplePath;

    HandlerRef ref;
    ref.clsid = clsid;
    ref.registryPath = L"debug";

    std::vector<std::wstring> paths = { samplePath };
    HandlerCapture capture;
    if (!CaptureHandlerWithMenu(paths, ParentFolderOf(samplePath), ref, SceneFile, capture))
    {
        out.failReason = capture.failReason;
        return false;
    }

    for (MenuInventoryItem& item : capture.items)
    {
        item.scenes = SceneFile;
        item.exts.push_back(ext);
        ApplyScope(item.children, SceneFile, ext);
    }
    out.items = std::move(capture.items);
    out.ok = true;
    return true;
}

bool CollectExtensionInventory(const std::wstring& sampleDir,
                               const std::wstring& ext,
                               MenuInventoryExtension& out)
{
    out = MenuInventoryExtension();
    out.ext = ext;

    const std::wstring samplePath = sampleDir + L"\\menu-inventory-sample" + ext;
    HANDLE file = ::CreateFileW(samplePath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        out.failReason = L"创建样本文件失败";
        return false;
    }
    ::CloseHandle(file);
    out.samplePath = samplePath;

    // 先采静态 verb：它们没有 CLSID，但同样有 owner（命令 EXE/注册表路径）。
    std::set<std::wstring> staticSeen;
    CaptureStaticVerbs(ext, out.items, staticSeen);

    // 再逐个 handler 采；每个 handler 单独一个 IContextMenu 实例。
    std::vector<HandlerRef> handlers = EnumerateFileHandlers(ext);
    for (const HandlerRef& ref : handlers)
    {
        HandlerCapture capture;
        std::vector<std::wstring> paths = { samplePath };
        if (!CaptureHandlerWithMenu(paths, ParentFolderOf(samplePath), ref, SceneFile, capture))
        {
            // 采不到不算整份失败；有些扩展必须在 Explorer 宿主里才初始化。
            Logger::Info(std::wstring(L"右键清单：跳过 handler ") + ref.clsid +
                         L"（" + capture.failReason + L"）");
            continue;
        }

        for (MenuInventoryItem& item : capture.items)
        {
            item.scenes = SceneFile;
            item.exts.push_back(ext);
            ApplyScope(item.children, SceneFile, ext);
        }
        out.items.insert(out.items.end(), capture.items.begin(), capture.items.end());
    }

    // 再采 Win11 现代菜单（WinRAR 7.20 这类 LegacyMenu=0 的扩展只在这里出现）。
    {
        std::vector<std::wstring> paths = { samplePath };
        CapturePackagedCommands(paths, ParentFolderOf(samplePath), SceneFile, ext, out.items);
    }

    out.ok = !out.items.empty();
    if (!out.ok) out.failReason = L"没有采到任何菜单项";
    return out.ok;
}

// ---------------------------------------------------------------------------
// 对外：缓存
// ---------------------------------------------------------------------------

std::wstring MenuInventoryPath()
{
    const std::wstring appData = AppDataDir();
    if (appData.empty()) return std::wstring();
    const std::wstring dir = appData + L"\\FileExplorer";
    ::SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir + L"\\menu-inventory.json";
}

bool SaveMenuInventory(const MenuInventoryReport& report)
{
    const std::wstring path = MenuInventoryPath();
    if (path.empty()) return false;

    json root;
    root["version"] = kInventoryVersion;
    root["scannedAt"] = ToUtf8(report.scannedAt);

    json extensions = json::array();
    for (const MenuInventoryExtension& extension : report.extensions)
    {
        json j;
        j["ext"] = ToUtf8(extension.ext);
        j["ok"] = extension.ok;
        j["failReason"] = ToUtf8(extension.failReason);
        j["samplePath"] = ToUtf8(extension.samplePath);
        json items = json::array();
        for (const MenuInventoryItem& item : extension.items) items.push_back(ItemToJson(item));
        j["items"] = items;
        extensions.push_back(j);
    }
    root["extensions"] = extensions;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << root.dump(2);
    return out.good();
}

bool LoadMenuInventory(MenuInventoryReport& report)
{
    report = MenuInventoryReport();

    const std::wstring path = MenuInventoryPath();
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    json root;
    try { in >> root; }
    catch (const std::exception& e)
    {
        Logger::Warn(std::wstring(L"右键清单解析失败：") + FromUtf8(e.what()));
        return false;
    }

    if (root.value("version", 0) != kInventoryVersion) return false;

    report.scannedAt = FromUtf8(root.value("scannedAt", std::string()));
    if (root.contains("extensions") && root["extensions"].is_array())
    {
        for (const auto& j : root["extensions"])
        {
            MenuInventoryExtension extension;
            extension.ext = FromUtf8(j.value("ext", std::string()));
            extension.ok = j.value("ok", false);
            extension.failReason = FromUtf8(j.value("failReason", std::string()));
            extension.samplePath = FromUtf8(j.value("samplePath", std::string()));
            if (j.contains("items") && j["items"].is_array())
            {
                for (const auto& item : j["items"]) extension.items.push_back(ItemFromJson(item));
            }
            report.extensions.push_back(std::move(extension));
        }
    }
    return true;
}

const MenuInventoryItem* FindInventoryItem(const MenuInventoryReport& report,
                                           const std::wstring& itemId)
{
    struct Finder
    {
        const std::wstring& id;

        const MenuInventoryItem* Find(const std::vector<MenuInventoryItem>& items) const
        {
            for (const MenuInventoryItem& item : items)
            {
                if (item.id == id) return &item;
                if (const MenuInventoryItem* child = Find(item.children)) return child;
            }
            return nullptr;
        }
    } finder{ itemId };

    for (const MenuInventoryExtension& extension : report.extensions)
    {
        if (const MenuInventoryItem* found = finder.Find(extension.items)) return found;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 对外：点击执行
// ---------------------------------------------------------------------------

namespace
{

std::wstring QuoteCommandArgument(const std::wstring& text)
{
    if (text.find(L' ') == std::wstring::npos && text.find(L'\t') == std::wstring::npos)
    {
        return text;
    }
    return L"\"" + text + L"\"";
}

void ReplaceAll(std::wstring& text, const std::wstring& from, const std::wstring& to)
{
    if (from.empty()) return;
    size_t position = 0;
    while ((position = text.find(from, position)) != std::wstring::npos)
    {
        text.replace(position, from.size(), to);
        position += to.size();
    }
}

std::wstring Expand(const std::wstring& text)
{
    if (text.empty()) return text;

    DWORD size = ::ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
    if (size == 0) return text;
    std::wstring result(size, L'\0');
    if (::ExpandEnvironmentStringsW(text.c_str(), result.data(), size) == 0) return text;
    while (!result.empty() && result.back() == L'\0') result.pop_back();
    return result;
}

bool RunCommandLine(const std::wstring& commandLine,
                    const std::wstring& targetPath,
                    const std::wstring& workingDir,
                    std::wstring* error)
{
    if (commandLine.empty())
    {
        if (error != nullptr) *error = L"静态 verb 没有命令行";
        return false;
    }

    const std::wstring quotedTarget = QuoteCommandArgument(targetPath);
    std::wstring command = Expand(commandLine);
    ReplaceAll(command, L"%1", quotedTarget);
    ReplaceAll(command, L"%L", quotedTarget);
    ReplaceAll(command, L"%V", quotedTarget);
    ReplaceAll(command, L"%W", quotedTarget);
    ReplaceAll(command, L"%D", workingDir);

    // 有些注册表命令不写 %1，而是把文件路径作为第一个参数隐式传入。
    if (command.find(targetPath) == std::wstring::npos &&
        command.find(quotedTarget) == std::wstring::npos)
    {
        command += L" " + quotedTarget;
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    std::wstring mutableCommand = command;
    const BOOL started = ::CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr,
                                           FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr,
                                           workingDir.empty() ? nullptr : workingDir.c_str(),
                                           &startup, &process);
    if (!started)
    {
        if (error != nullptr)
        {
            *error = L"CreateProcess 失败，错误码 " + std::to_wstring(::GetLastError());
        }
        return false;
    }

    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return true;
}

bool RunShellVerb(const MenuInventoryItem& item,
                  const std::wstring& targetPath,
                  HWND owner,
                  std::wstring* error)
{
    std::wstring verb = item.invoke.verb;
    if (verb.empty()) verb = L"open";

    SHELLEXECUTEINFOW execute = {};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    execute.hwnd = owner;
    execute.lpVerb = verb.c_str();
    execute.lpFile = targetPath.c_str();
    execute.nShow = SW_SHOWNORMAL;

    if (!::ShellExecuteExW(&execute))
    {
        if (error != nullptr)
        {
            *error = L"ShellExecuteEx 失败，错误码 " + std::to_wstring(::GetLastError());
        }
        return false;
    }
    return true;
}

bool InvokeShellHandler(const MenuInventoryItem& item,
                        const std::vector<std::wstring>& targetPaths,
                        const std::wstring& parentFolder,
                        HWND owner,
                        std::wstring* error)
{
    if (item.invoke.clsid.empty())
    {
        if (error != nullptr) *error = L"shell handler 缺少 CLSID";
        return false;
    }
    // 规范动词名优先；动词名缺失时才回退命令偏移。两者都没有才报错。
    if (item.invoke.verb.empty() && item.invoke.commandOffset < 0)
    {
        if (error != nullptr) *error = L"shell handler 缺少动词名 / 命令偏移";
        return false;
    }

    HRESULT hr = S_OK;

    CLSID clsid = {};
    if (FAILED(::CLSIDFromString(item.invoke.clsid.c_str(), &clsid)))
    {
        if (error != nullptr) *error = L"CLSID 格式错误";
        return false;
    }

    ComPtr<IUnknown> unknown;
    {
        hr = ::CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_PPV_ARGS(unknown.GetAddressOf()));
        if (FAILED(hr) || unknown == nullptr)
        {
            if (error != nullptr) *error = L"CoCreateInstance 失败";
            return false;
        }
    }

    ComPtr<IContextMenu> contextMenu;
    if (FAILED(unknown.As(&contextMenu)) || contextMenu == nullptr)
    {
        if (error != nullptr) *error = L"没有 IContextMenu";
        return false;
    }

    ShellExtContext context;
    std::wstring initError;
    {
        hr = InitializeShellExt(unknown.Get(), targetPaths, parentFolder, context, &initError);
    }
    if (FAILED(hr))
    {
        if (error != nullptr) *error = initError.empty() ? L"Initialize 失败" : initError;
        return false;
    }

    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr)
    {
        if (error != nullptr) *error = L"CreatePopupMenu 失败";
        return false;
    }

    const int idFirst = 1;
    const int idLast = 0x7FFF;
    {
        hr = contextMenu->QueryContextMenu(menu, 0, idFirst, idLast, CMF_NORMAL);
    }

    // 即使 QueryContextMenu 返回失败，个别扩展仍能响应 InvokeCommand；
    // 这里只在菜单完全没有命令时提前退出，避免把真实失败吞掉。
    if (FAILED(hr) && ::GetMenuItemCount(menu) == 0)
    {
        ::DestroyMenu(menu);
        if (error != nullptr) *error = L"QueryContextMenu 失败";
        return false;
    }

    // 优先用采集时取到的规范动词名调用；动词名缺失才回退命令偏移。
    // 偏移依赖两次 QueryContextMenu 的命令排列一致，动态菜单（如 WPS）下会错位。
    const bool useVerb = !item.invoke.verb.empty();

    CMINVOKECOMMANDINFOEX invoke = {};
    invoke.cbSize = sizeof(invoke);
    invoke.fMask = CMIC_MASK_UNICODE;
    invoke.hwnd = owner;
    invoke.nShow = SW_SHOWNORMAL;
    if (useVerb)
    {
        invoke.lpVerb = nullptr;
        invoke.lpVerbW = const_cast<LPWSTR>(item.invoke.verb.c_str());
    }
    else
    {
        invoke.lpVerb = MAKEINTRESOURCEA(static_cast<UINT>(item.invoke.commandOffset));
        invoke.lpVerbW = MAKEINTRESOURCEW(static_cast<UINT>(item.invoke.commandOffset));
    }

    {
        hr = contextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
    }
    ::DestroyMenu(menu);

    if (FAILED(hr))
    {
        if (error != nullptr) *error = L"InvokeCommand 失败，HRESULT=" + Logger::FormatHresult(hr);
        return false;
    }
    return true;
}

bool InvokePackagedCommand(const MenuInventoryItem& item,
                           const std::vector<std::wstring>& targetPaths,
                           const std::wstring& parentFolder,
                           HWND owner,
                           std::wstring* error)
{
    (void)owner;
    if (item.invoke.clsid.empty())
    {
        if (error != nullptr) *error = L"modern command 缺少 CLSID";
        return false;
    }

    CLSID clsid = {};
    if (FAILED(::CLSIDFromString(item.invoke.clsid.c_str(), &clsid)))
    {
        if (error != nullptr) *error = L"CLSID 格式错误";
        return false;
    }

    ComPtr<IExplorerCommand> current;
    if (FAILED(::CoCreateInstance(clsid, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(current.GetAddressOf()))) || current == nullptr)
    {
        if (error != nullptr) *error = L"IExplorerCommand 创建失败";
        return false;
    }

    ComPtr<IShellItemArray> items;
    if (!MakeShellItemArray(targetPaths, parentFolder, items))
    {
        if (error != nullptr) *error = L"无法为目标路径创建 IShellItemArray";
        return false;
    }

    // invoke.verb 存的是子命令索引路径，例如 "0/2"；空表示顶层命令。
    const std::wstring path = item.invoke.verb;
    size_t begin = 0;
    while (begin < path.size())
    {
        const size_t slash = path.find(L'/', begin);
        const std::wstring token = path.substr(begin, slash == std::wstring::npos
            ? std::wstring::npos : slash - begin);
        if (token.empty() || token.find_first_not_of(L"0123456789") != std::wstring::npos)
        {
            if (error != nullptr) *error = L"modern command 子命令路径无效";
            return false;
        }

        const int wanted = std::stoi(token);
        ComPtr<IEnumExplorerCommand> enumerator;
        if (FAILED(current->EnumSubCommands(enumerator.GetAddressOf())) || enumerator == nullptr)
        {
            if (error != nullptr) *error = L"modern command 没有子命令";
            return false;
        }

        ComPtr<IExplorerCommand> next;
        for (int index = 0; index <= wanted; ++index)
        {
            IExplorerCommand* raw = nullptr;
            ULONG fetched = 0;
            if (FAILED(enumerator->Next(1, &raw, &fetched)) || fetched != 1 || raw == nullptr)
            {
                if (error != nullptr) *error = L"modern command 子命令索引越界";
                return false;
            }
            if (index == wanted)
            {
                next.Attach(raw);
            }
            else
            {
                raw->Release();
            }
        }
        current.Swap(next);
        begin = (slash == std::wstring::npos) ? path.size() : slash + 1;
    }

    const HRESULT hr = current->Invoke(items.Get(), nullptr);
    if (FAILED(hr))
    {
        if (error != nullptr) *error = L"IExplorerCommand::Invoke 失败，HRESULT=" + Logger::FormatHresult(hr);
        return false;
    }
    return true;
}

} // namespace

bool CollectSceneInventory(unsigned int scene,
                           const std::wstring& sampleDir,
                           std::vector<MenuInventoryItem>& out,
                           std::wstring* error)
{
    return CollectSceneInventoryInternal(scene, sampleDir, out, error);
}

bool CaptureOwnerMenu(MenuInvokeKind kind,
                      const std::wstring& clsid,
                      const std::vector<std::wstring>& paths,
                      const std::wstring& parentFolder,
                      unsigned int scene,
                      std::vector<MenuInventoryItem>& out,
                      std::wstring* error)
{
    return CaptureOwnerMenuInternal(kind, clsid, paths, parentFolder, scene, out, error);
}

bool InvokeInventoryItem(const MenuInventoryItem& item,
                         const std::vector<std::wstring>& targetPaths,
                         const std::wstring& workingDir,
                         HWND owner,
                         std::wstring* error)
{
    if (error != nullptr) error->clear();
    if (targetPaths.empty() && item.invoke.kind != MenuInvokeKind::WindowsBuiltin)
    {
        // 空白区菜单没有选中项：经典 handler 用父文件夹 PIDL；
        // 现代命令把父文件夹本身当作 IShellItemArray 的一项。
        const bool canUseParent = item.invoke.kind == MenuInvokeKind::ShellHandler ||
                                  item.invoke.kind == MenuInvokeKind::PackagedCommand;
        if (!canUseParent || workingDir.empty())
        {
            if (error != nullptr) *error = L"目标路径为空";
            return false;
        }
    }

    switch (item.invoke.kind)
    {
    case MenuInvokeKind::StaticVerb:
    {
        // 静态 verb 一次只能带一个文件；多选时逐个执行。
        for (const std::wstring& targetPath : targetPaths)
        {
            bool ok = false;
            if (!item.invoke.commandLine.empty())
            {
                ok = RunCommandLine(item.invoke.commandLine, targetPath, workingDir, error);
            }
            else
            {
                ok = RunShellVerb(item, targetPath, owner, error);
            }
            if (!ok) return false;
        }
        return true;
    }

    case MenuInvokeKind::ShellHandler:
    {
        const std::wstring parentFolder = !workingDir.empty()
            ? workingDir
            : (targetPaths.empty() ? std::wstring() : ParentFolderOf(targetPaths.front()));
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool uninitialize = (com == S_OK);   // S_FALSE = 本线程已初始化，不能替别人收尾
        const bool result = InvokeShellHandler(item, targetPaths, parentFolder, owner, error);
        if (uninitialize) ::CoUninitialize();
        return result;
    }

    case MenuInvokeKind::WindowsBuiltin:
        for (const std::wstring& targetPath : targetPaths)
        {
            if (!RunShellVerb(item, targetPath, owner, error)) return false;
        }
        return !targetPaths.empty();

    case MenuInvokeKind::PackagedCommand:
    {
        std::vector<std::wstring> packagedPaths = targetPaths;
        if (packagedPaths.empty() && !workingDir.empty()) packagedPaths.push_back(workingDir);

        const std::wstring parentFolder = !workingDir.empty()
            ? workingDir
            : (targetPaths.empty() ? std::wstring() : ParentFolderOf(targetPaths.front()));

        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool uninitialize = (com == S_OK);   // S_FALSE = 本线程已初始化，不能替别人收尾
        const bool result = InvokePackagedCommand(item, packagedPaths, parentFolder, owner, error);
        if (uninitialize) ::CoUninitialize();
        return result;
    }
    }

    if (error != nullptr) *error = L"未知的执行类型";
    return false;
}

bool InvokeInventoryItem(const MenuInventoryItem& item,
                         const std::wstring& targetPath,
                         const std::wstring& workingDir,
                         HWND owner,
                         std::wstring* error)
{
    std::vector<std::wstring> paths;
    if (!targetPath.empty()) paths.push_back(targetPath);
    return InvokeInventoryItem(item, paths, workingDir, owner, error);
}

} // namespace shell
