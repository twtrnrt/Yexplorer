// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ShellClipboard.cpp
// ============================================================================

#include "ShellClipboard.h"

#include <ole2.h>       // OleGetClipboard、IDataObject（"虚拟文件"要问剪贴板要内容）
#include <shlobj.h>     // DROPFILES、DROPEFFECT_*
#include <shellapi.h>   // DragQueryFileW

#include <algorithm>
#include <chrono>
#include <stdio.h>      // swprintf_s（把 HRESULT 写成十六进制）

namespace shellclip
{

namespace
{

// 剪贴板同一时间只能被一个程序占用，遇到占用就等一会儿重试
bool OpenClipboardWithRetry(HWND owner)
{
    for (int attempt = 0; attempt < 10; ++attempt)
    {
        if (::OpenClipboard(owner))
        {
            return true;
        }
        ::Sleep(50);
    }
    return false;
}

// "Preferred DropEffect" 这个格式名，系统里就是按字符串注册的
const wchar_t* kPreferredDropEffect = L"Preferred DropEffect";

} // namespace

// ============================================================================
// 写一段纯文字进剪贴板（「复制文件路径」用）
//
// 和 WriteFiles 分开：那个放的是 CF_HDROP（文件清单），
// 这个放的是 CF_UNICODETEXT（一段文字），记事本、输入框之类的地方都能直接粘贴。
// ============================================================================
bool WriteText(const std::wstring& text, HWND owner)
{
    if (text.empty())
    {
        return false;
    }

    if (!OpenClipboardWithRetry(owner))
    {
        return false;
    }

    ::EmptyClipboard();

    // 末尾要带一个 \0，按字符数算字节
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);

    HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem == nullptr)
    {
        ::CloseClipboard();
        return false;
    }

    void* data = ::GlobalLock(hMem);
    if (data == nullptr)
    {
        ::GlobalFree(hMem);
        ::CloseClipboard();
        return false;
    }

    ::memcpy(data, text.c_str(), bytes);
    ::GlobalUnlock(hMem);

    // 放进去成功之后这块内存归系统管，不能自己释放
    if (::SetClipboardData(CF_UNICODETEXT, hMem) == nullptr)
    {
        ::GlobalFree(hMem);
        ::CloseClipboard();
        return false;
    }

    ::CloseClipboard();
    return true;
}

// ============================================================================
// 写剪贴板（文件）
// ============================================================================
bool WriteFiles(const std::vector<std::wstring>& paths, bool isCut, HWND owner)
{
    if (paths.empty())
    {
        return false;
    }

    if (!OpenClipboardWithRetry(owner))
    {
        return false;
    }

    ::EmptyClipboard();

    // ------------------------------------------------------------
    // 1) CF_HDROP
    //    内存里的布局：DROPFILES 结构体打头，后面跟一串 UTF-16 路径，
    //    每条以 \0 结尾，整个列表最后再补一个 \0。
    // ------------------------------------------------------------
    size_t totalChars = 0;
    for (const auto& path : paths)
    {
        totalChars += path.size() + 1;   // 每条路径自身的结尾 \0
    }
    totalChars += 1;                     // 整个列表的结尾 \0

    const size_t headerSize = sizeof(DROPFILES);
    const size_t totalSize  = headerSize + totalChars * sizeof(wchar_t);

    HGLOBAL hDrop = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalSize);
    if (hDrop == nullptr)
    {
        ::CloseClipboard();
        return false;
    }

    DROPFILES* dropFiles = static_cast<DROPFILES*>(::GlobalLock(hDrop));
    if (dropFiles == nullptr)
    {
        ::GlobalFree(hDrop);
        ::CloseClipboard();
        return false;
    }

    dropFiles->pFiles = static_cast<DWORD>(headerSize);   // 路径数据从结构体后面开始
    dropFiles->fWide  = TRUE;                             // 用宽字符

    wchar_t* cursor = reinterpret_cast<wchar_t*>(
        reinterpret_cast<BYTE*>(dropFiles) + headerSize);

    for (const auto& path : paths)
    {
        ::wcscpy_s(cursor, path.size() + 1, path.c_str());
        cursor += path.size() + 1;
    }
    *cursor = L'\0';

    ::GlobalUnlock(hDrop);

    if (::SetClipboardData(CF_HDROP, hDrop) == nullptr)
    {
        // 没交出去，内存还是我们的，得自己释放
        ::GlobalFree(hDrop);
        ::CloseClipboard();
        return false;
    }
    // 交出去之后所有权归系统，这里绝对不能再 GlobalFree，否则会崩

    // ------------------------------------------------------------
    // 2) Preferred DropEffect：告诉接收方这次是复制还是剪切
    // ------------------------------------------------------------
    UINT effectFormat = ::RegisterClipboardFormatW(kPreferredDropEffect);
    if (effectFormat != 0)
    {
        HGLOBAL hEffect = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
        if (hEffect != nullptr)
        {
            DWORD* effect = static_cast<DWORD*>(::GlobalLock(hEffect));
            if (effect != nullptr)
            {
                *effect = isCut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
                ::GlobalUnlock(hEffect);

                if (::SetClipboardData(effectFormat, hEffect) == nullptr)
                {
                    ::GlobalFree(hEffect);
                }
            }
            else
            {
                ::GlobalFree(hEffect);
            }
        }
    }

    ::CloseClipboard();
    return true;
}

// ============================================================================
// 读剪贴板
// ============================================================================
bool ReadFiles(std::vector<std::wstring>& paths, bool& isCut, HWND owner)
{
    paths.clear();
    isCut = false;

    if (!::IsClipboardFormatAvailable(CF_HDROP))
    {
        return false;
    }

    if (!OpenClipboardWithRetry(owner))
    {
        return false;
    }

    bool ok = false;

    // ------------------------------------------------------------
    // 1) 文件路径列表
    //    注意：GetClipboardData 给的内存只有到 CloseClipboard 之前有效，
    //    所以所有字符串必须在关闭剪贴板之前复制出来。
    // ------------------------------------------------------------
    HDROP drop = static_cast<HDROP>(::GetClipboardData(CF_HDROP));
    if (drop != nullptr)
    {
        UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);

        for (UINT i = 0; i < count; ++i)
        {
            UINT length = ::DragQueryFileW(drop, i, nullptr, 0);
            if (length == 0)
            {
                continue;
            }

            std::wstring path(static_cast<size_t>(length) + 1, L'\0');
            if (::DragQueryFileW(drop, i, path.data(), length + 1) > 0)
            {
                path.resize(length);
                paths.push_back(path);
            }
        }

        ok = !paths.empty();
    }

    // ------------------------------------------------------------
    // 2) 首选效果：是复制还是剪切
    // ------------------------------------------------------------
    UINT effectFormat = ::RegisterClipboardFormatW(kPreferredDropEffect);
    if (effectFormat != 0)
    {
        HGLOBAL hEffect = static_cast<HGLOBAL>(::GetClipboardData(effectFormat));
        if (hEffect != nullptr)
        {
            DWORD* effect = static_cast<DWORD*>(::GlobalLock(hEffect));
            if (effect != nullptr)
            {
                isCut = (*effect & DROPEFFECT_MOVE) != 0;
                ::GlobalUnlock(hEffect);
            }
        }
    }

    ::CloseClipboard();
    return ok;
}

// ============================================================================
// 查询 / 清空
// ============================================================================

bool HasFiles()
{
    // 先看最常用的真实路径格式：本程序/资源管理器/桌面复制出来的都是这个，
    // 命中就直接返回，不多做任何检查（右键菜单还是零成本）
    if (::IsClipboardFormatAvailable(CF_HDROP))
    {
        return true;
    }

    // 再问一句"是不是虚拟文件"（远程桌面复制过来的那种）
    return HasVirtualFiles();
}

bool Clear(HWND owner)
{
    if (!OpenClipboardWithRetry(owner))
    {
        return false;
    }
    ::EmptyClipboard();
    ::CloseClipboard();
    return true;
}

// ============================================================================
// 虚拟文件（远程桌面复制过来的那类）
//
// 下面这些格式名是 OLE 规定的，系统里按字符串注册，名字不能改：
//   FileGroupDescriptorW / FileGroupDescriptorA  清单（宽字符版 / 老式 ANSI 版）
//   FileContents                                 内容流（按清单序号取第几个）
//   FileNameW                                    单个文件名（有些老程序只给这个，这里不用）
// ============================================================================

namespace
{

const wchar_t* kDescriptorFormatW = L"FileGroupDescriptorW";
const wchar_t* kDescriptorFormatA = L"FileGroupDescriptorA";
const wchar_t* kContentsFormat    = L"FileContents";

// 一次读多少（64KB：远程桌面通道上太小会拖慢，太大占内存）
const size_t kCopyChunkSize = 64 * 1024;

// 进度回调的节流间隔：太频繁会让状态栏一直闪
const long long kProgressIntervalMs = 150;

// 把 Win32 错误码 / HRESULT 转成能看懂的提示
std::wstring SystemErrorText(DWORD code)
{
    wchar_t buffer[512] = {};

    ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                     nullptr, code, 0, buffer, 512, nullptr);

    std::wstring text = buffer;

    // 系统给的文字末尾常带 \r\n，去掉
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
    {
        text.pop_back();
    }

    if (text.empty())
    {
        text = L"错误码 " + std::to_wstring(code);
    }

    return text;
}

// HRESULT 按十六进制写出来（日志/提示里排查时比十进制好认）
std::wstring HresultHex(HRESULT hr)
{
    wchar_t buffer[24] = {};

    ::swprintf_s(buffer, L"0x%08lX", static_cast<unsigned long>(hr));

    return buffer;
}

// 超过 260 字符的路径，普通 Win32 调用会失败，得换成 \\?\ 写法。
// 和 MainWnd / ShellOperations 里的处理保持一致。
std::wstring MakeLongPath(const std::wstring& path)
{
    if (path.size() < 260 || path.size() >= 32767)
    {
        return path;
    }

    if (path.compare(0, 4, L"\\\\?\\") == 0)
    {
        return path;
    }

    if (path.size() > 1 && path[1] == L':')
    {
        return L"\\\\?\\" + path;
    }

    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\')
    {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }

    return path;
}

// 把清单里的名字清成"能当相对路径用"的形式
//
// 为什么要清：
//   1) RDP 复制整个文件夹时会带 "子目录\文件" 这种相对路径，分隔符可能是 / 也可能是 \；
//   2) 名字里可能出现 Windows 不允许的字符（清单是对方系统给的，不保证干净）；
//   3) 必须挡住 ".."，否则恶意/异常来源能让我们写到目标目录外面去。
std::wstring SanitizeRelativeName(const std::wstring& raw)
{
    std::wstring result;
    std::wstring segment;

    auto flushSegment = [&]()
    {
        // 每段结尾的空格和点在 Windows 上不合法，去掉
        while (!segment.empty() && (segment.back() == L' ' || segment.back() == L'.'))
        {
            segment.pop_back();
        }

        // 空段、"."、".." 一律丢掉（"." 和 ".." 是防越界的关键）
        if (segment.empty() || segment == L"." || segment == L"..")
        {
            segment.clear();
            return;
        }

        // 单段过长（超过 200 字符）就截断，避免拼出超长路径
        if (segment.size() > 200)
        {
            segment.resize(200);
        }

        if (!result.empty())
        {
            result += L'\\';
        }

        result += segment;
        segment.clear();
    };

    for (wchar_t ch : raw)
    {
        if (ch == L'\\' || ch == L'/')
        {
            flushSegment();
            continue;
        }

        // 控制字符和文件名里不允许的字符，换成下划线（换名总比失败好）
        if (ch < 32 || ch == L'<' || ch == L'>' || ch == L':' ||
            ch == L'"' || ch == L'|' || ch == L'?' || ch == L'*')
        {
            segment += L'_';
            continue;
        }

        segment += ch;
    }

    flushSegment();

    return result;
}

// 把一整个 HGLOBAL 一次性写到文件里（少数来源用 TYMED_HGLOBAL 给内容）
bool WriteWholeBlock(HANDLE file, HGLOBAL block, std::wstring& error)
{
    const SIZE_T total = ::GlobalSize(block);

    const void* data = ::GlobalLock(block);

    if (data == nullptr)
    {
        error = L"锁定剪贴板数据失败";
        return false;
    }

    const BYTE* bytes   = static_cast<const BYTE*>(data);
    SIZE_T      written = 0;

    while (written < total)
    {
        const DWORD request = static_cast<DWORD>(
            std::min<SIZE_T>(total - written, static_cast<SIZE_T>(kCopyChunkSize)));

        DWORD nowWritten = 0;

        if (!::WriteFile(file, bytes + written, request, &nowWritten, nullptr) ||
            nowWritten == 0)
        {
            ::GlobalUnlock(block);
            error = L"写入文件失败：" + SystemErrorText(::GetLastError());
            return false;
        }

        written += nowWritten;
    }

    ::GlobalUnlock(block);
    return true;
}

} // namespace（虚拟文件的小工具）

bool HasVirtualFiles()
{
    // RegisterClipboardFormat 是查系统里有没有这个名字，重复调用的成本很低，
    // 但菜单每次弹出都会走这里，所以缓存成静态的（C++11 起局部静态初始化是线程安全的）
    static const CLIPFORMAT descriptorW = static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(kDescriptorFormatW));
    static const CLIPFORMAT descriptorA = static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(kDescriptorFormatA));

    if (::IsClipboardFormatAvailable(descriptorW))
    {
        return true;
    }

    // 老式 ANSI 版清单（少见，但有些第三方程序还在用）
    return ::IsClipboardFormatAvailable(descriptorA) != FALSE;
}

VirtualFileSession::VirtualFileSession() = default;

VirtualFileSession::~VirtualFileSession()
{
    Close();
}

void VirtualFileSession::Close()
{
    m_files.clear();
    m_lastError.clear();

    if (m_dataObject != nullptr)
    {
        // OleGetClipboard 返回的对象带一次引用，用完要还回去
        m_dataObject->Release();
        m_dataObject = nullptr;
    }
}

bool VirtualFileSession::IsOpen() const
{
    return m_dataObject != nullptr;
}

size_t VirtualFileSession::Count() const
{
    return m_files.size();
}

const VirtualFileInfo& VirtualFileSession::Item(size_t index) const
{
    return m_files[index];
}

const std::wstring& VirtualFileSession::LastError() const
{
    return m_lastError;
}

bool VirtualFileSession::Open()
{
    Close();

    // OleGetClipboard 拿的是"剪贴板的数据对象"，能力比 GetClipboardData 强：
    // 内容流（FileContents）是延迟生成的，只有通过它才能按需取到
    HRESULT hr = ::OleGetClipboard(&m_dataObject);

    if (FAILED(hr) || m_dataObject == nullptr)
    {
        m_dataObject = nullptr;
        m_lastError = hr == CLIPBRD_E_CANT_OPEN ? L"剪贴板被别的程序占用了，请稍后再试"
                                                : L"读取剪贴板失败：" + SystemErrorText(static_cast<DWORD>(hr));
        return false;
    }

    // 清单优先用宽字符版；没有再用 ANSI 版兜底
    if (ReadDescriptor(true))
    {
        return true;
    }

    const std::wstring wideError = m_lastError;

    if (ReadDescriptor(false))
    {
        return true;
    }

    // 两个都失败：把宽字符版的原因留下（那是最该看的）
    m_lastError = wideError;
    return false;
}

bool VirtualFileSession::ReadDescriptor(bool wideVersion)
{
    FORMATETC format = {};
    format.cfFormat = static_cast<CLIPFORMAT>(
        ::RegisterClipboardFormatW(wideVersion ? kDescriptorFormatW : kDescriptorFormatA));
    format.dwAspect = DVASPECT_CONTENT;
    format.lindex   = -1;                       // 清单是"整个一组"，不是第几个
    format.tymed    = TYMED_HGLOBAL;            // 清单在全局内存块里

    STGMEDIUM medium = {};

    HRESULT hr = m_dataObject->GetData(&format, &medium);

    if (FAILED(hr) || medium.tymed != TYMED_HGLOBAL || medium.hGlobal == nullptr)
    {
        if (SUCCEEDED(hr))
        {
            ::ReleaseStgMedium(&medium);
        }

        m_lastError = L"剪贴板里没有文件清单（HRESULT=" + HresultHex(hr) + L"）";
        return false;
    }

    m_files.clear();

    if (wideVersion)
    {
        const FILEGROUPDESCRIPTORW* group =
            static_cast<const FILEGROUPDESCRIPTORW*>(::GlobalLock(medium.hGlobal));

        if (group == nullptr)
        {
            ::ReleaseStgMedium(&medium);
            m_lastError = L"锁定剪贴板文件清单失败";
            return false;
        }

        for (UINT i = 0; i < group->cItems; ++i)
        {
            const FILEDESCRIPTORW& item = group->fgd[i];

            VirtualFileInfo info;
            info.name = SanitizeRelativeName(item.cFileName);

            if (info.name.empty())
            {
                // 名字清完是空的，这种项没法落地，跳过（不然会写出一个无名文件）
                continue;
            }

            if ((item.dwFlags & FD_ATTRIBUTES) != 0)
            {
                info.attributes    = item.dwFileAttributes;
                info.hasAttributes = true;
            }

            if ((item.dwFlags & FD_FILESIZE) != 0)
            {
                info.size = (static_cast<LONGLONG>(item.nFileSizeHigh) << 32) |
                            static_cast<LONGLONG>(item.nFileSizeLow);
                info.hasSize = true;
            }

            if ((item.dwFlags & FD_WRITESTIME) != 0)
            {
                info.lastWriteTime    = item.ftLastWriteTime;
                info.hasLastWriteTime = true;
            }

            m_files.push_back(info);
        }

        ::GlobalUnlock(medium.hGlobal);
        ::ReleaseStgMedium(&medium);
    }
    else
    {
        const FILEGROUPDESCRIPTORA* group =
            static_cast<const FILEGROUPDESCRIPTORA*>(::GlobalLock(medium.hGlobal));

        if (group == nullptr)
        {
            ::ReleaseStgMedium(&medium);
            m_lastError = L"锁定剪贴板文件清单失败";
            return false;
        }

        for (UINT i = 0; i < group->cItems; ++i)
        {
            const FILEDESCRIPTORA& item = group->fgd[i];

            // ANSI 版的名字按系统默认代码页解（这类老程序给的就是本机代码页）
            std::string  ansiName = item.cFileName;
            std::wstring wideName;

            if (!ansiName.empty())
            {
                const int length = ::MultiByteToWideChar(CP_ACP, 0, ansiName.c_str(),
                                                         static_cast<int>(ansiName.size()),
                                                         nullptr, 0);
                if (length > 0)
                {
                    wideName.resize(static_cast<size_t>(length));
                    ::MultiByteToWideChar(CP_ACP, 0, ansiName.c_str(),
                                          static_cast<int>(ansiName.size()),
                                          wideName.data(), length);
                }
            }

            VirtualFileInfo info;
            info.name = SanitizeRelativeName(wideName);

            if (info.name.empty())
            {
                continue;
            }

            if ((item.dwFlags & FD_ATTRIBUTES) != 0)
            {
                info.attributes    = item.dwFileAttributes;
                info.hasAttributes = true;
            }

            if ((item.dwFlags & FD_FILESIZE) != 0)
            {
                info.size = (static_cast<LONGLONG>(item.nFileSizeHigh) << 32) |
                            static_cast<LONGLONG>(item.nFileSizeLow);
                info.hasSize = true;
            }

            if ((item.dwFlags & FD_WRITESTIME) != 0)
            {
                info.lastWriteTime    = item.ftLastWriteTime;
                info.hasLastWriteTime = true;
            }

            m_files.push_back(info);
        }

        ::GlobalUnlock(medium.hGlobal);
        ::ReleaseStgMedium(&medium);
    }

    if (m_files.empty())
    {
        m_lastError = L"剪贴板文件清单是空的";
        return false;
    }

    return true;
}

bool VirtualFileSession::Extract(size_t index, const std::wstring& destinationPath,
                                 const ProgressCallback& onProgress, std::wstring& error)
{
    error.clear();

    if (!IsOpen())
    {
        error = L"剪贴板数据对象还没打开";
        return false;
    }

    if (index >= m_files.size())
    {
        error = L"清单里没有第 " + std::to_wstring(index + 1) + L" 项";
        return false;
    }

    const VirtualFileInfo& info = m_files[index];

    // 清单里的目录项没有内容可取，只要把目录建出来
    if (info.hasAttributes && (info.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        const std::wstring longPath = MakeLongPath(destinationPath);

        if (::CreateDirectoryW(longPath.c_str(), nullptr))
        {
            return true;
        }

        const DWORD lastError = ::GetLastError();

        // 并发/上级已经建过，这里不算失败
        if (lastError == ERROR_ALREADY_EXISTS)
        {
            return true;
        }

        error = L"建文件夹失败：" + SystemErrorText(lastError);
        return false;
    }

    // 取内容：先按"流"要（远程桌面走的就是这一路），失败再按"一整块内存"要
    FORMATETC format = {};
    format.cfFormat = static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(kContentsFormat));
    format.dwAspect = DVASPECT_CONTENT;
    format.lindex   = static_cast<LONG>(index);

    STGMEDIUM medium = {};

    format.tymed = TYMED_ISTREAM;
    HRESULT hr = m_dataObject->GetData(&format, &medium);

    if (FAILED(hr))
    {
        format.tymed = TYMED_HGLOBAL;
        hr           = m_dataObject->GetData(&format, &medium);
    }

    if (FAILED(hr))
    {
        error = L"取文件内容失败（HRESULT=" + HresultHex(hr) + L"），"
                L"可能是远程会话已断开或剪贴板内容已失效";
        return false;
    }

    if (medium.tymed != TYMED_ISTREAM && medium.tymed != TYMED_HGLOBAL)
    {
        // TYMED_ISTORAGE 之类（复合文档）很少见，不猜着写，直接报错
        const DWORD unsupported = static_cast<DWORD>(medium.tymed);

        ::ReleaseStgMedium(&medium);

        error = L"剪贴板给的格式暂不支持（tymed=" + std::to_wstring(unsupported) + L"）";
        return false;
    }

    // 目标文件用 CREATE_NEW：绝不覆盖已有文件（重名由调用方先避开）
    const std::wstring longPath = MakeLongPath(destinationPath);

    HANDLE file = ::CreateFileW(longPath.c_str(),
                                GENERIC_WRITE,
                                0,                  // 独占：别人正在用就立刻失败，不要等
                                nullptr,
                                CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);

    if (file == INVALID_HANDLE_VALUE)
    {
        const DWORD lastError = ::GetLastError();

        ::ReleaseStgMedium(&medium);

        error = (lastError == ERROR_FILE_EXISTS || lastError == ERROR_ALREADY_EXISTS)
                ? L"目标位置已经有同名文件"
                : L"创建文件失败：" + SystemErrorText(lastError);
        return false;
    }

    bool                canceled = false;
    std::wstring        writeError;
    LONGLONG            written    = 0;
    LONGLONG            lastTickMs = 0;
    int                 lastPercent = -2;   // -2 表示"还没报过"

    // 进度百分比：来源没告诉大小时给 -1（界面显示"第 i/N 项"就够了）
    auto reportProgress = [&](bool force) -> bool
    {
        if (!onProgress)
        {
            return true;
        }

        const int percent = (info.hasSize && info.size > 0)
                            ? static_cast<int>(std::min<LONGLONG>(100, written * 100 / info.size))
                            : -1;

        const LONGLONG nowTick = ::GetTickCount64();

        if (!force && percent == lastPercent && nowTick - lastTickMs < kProgressIntervalMs)
        {
            return true;
        }

        lastPercent = percent;
        lastTickMs  = nowTick;

        return onProgress(percent);
    };

    if (medium.tymed == TYMED_ISTREAM)
    {
        IStream* stream = medium.pstm;

        std::vector<BYTE> buffer(kCopyChunkSize);

        while (true)
        {
            ULONG readBytes = 0;

            const HRESULT readHr = stream->Read(buffer.data(),
                                                static_cast<ULONG>(buffer.size()),
                                                &readBytes);

            if (readHr != S_OK && readHr != S_FALSE)
            {
                writeError = L"读剪贴板数据失败（HRESULT=" + HresultHex(readHr) + L"）";
                break;
            }

            if (readBytes > 0)
            {
                DWORD nowWritten = 0;

                if (!::WriteFile(file, buffer.data(), readBytes, &nowWritten, nullptr) ||
                    nowWritten != readBytes)
                {
                    writeError = L"写入文件失败：" + SystemErrorText(::GetLastError());
                    break;
                }

                written += nowWritten;
            }

            // S_FALSE 表示流里没有更多数据了
            if (readHr == S_FALSE || readBytes == 0)
            {
                break;
            }

            if (!reportProgress(false))
            {
                canceled = true;
                break;
            }
        }
    }
    else   // TYMED_HGLOBAL
    {
        if (!WriteWholeBlock(file, medium.hGlobal, writeError))
        {
            // 错误已经写进 writeError
        }
        else
        {
            written = static_cast<LONGLONG>(::GlobalSize(medium.hGlobal));
            canceled = !reportProgress(true);
        }
    }

    ::ReleaseStgMedium(&medium);
    ::CloseHandle(file);

    // 取消或被中断：把这半截文件删掉，不留垃圾
    if (canceled || !writeError.empty() || (info.hasSize && info.size >= 0 && written < info.size))
    {
        // 大小对不上也算失败（远程会话中途断了会读到提前结束的流）
        if (writeError.empty())
        {
            writeError = (info.hasSize && info.size >= 0 && written < info.size)
                         ? L"内容没有传完（收到 " + std::to_wstring(written) + L" / " +
                           std::to_wstring(info.size) + L" 字节）"
                         : std::wstring();
        }

        ::DeleteFileW(longPath.c_str());

        if (canceled)
        {
            error = L"已取消";
        }
        else if (!writeError.empty())
        {
            error = writeError;
        }

        return false;
    }

    // 按清单补属性和修改时间（失败不影响文件本身，忽略即可）
    if (info.hasLastWriteTime)
    {
        FILETIME zero = {};

        if (!(info.lastWriteTime.dwLowDateTime == zero.dwLowDateTime &&
              info.lastWriteTime.dwHighDateTime == zero.dwHighDateTime))
        {
            HANDLE attributeFile = ::CreateFileW(longPath.c_str(),
                                                 FILE_WRITE_ATTRIBUTES,
                                                 0, nullptr, OPEN_EXISTING,
                                                 FILE_ATTRIBUTE_NORMAL, nullptr);

            if (attributeFile != INVALID_HANDLE_VALUE)
            {
                ::SetFileTime(attributeFile, nullptr, nullptr, &info.lastWriteTime);
                ::CloseHandle(attributeFile);
            }
        }
    }

    if (info.hasAttributes)
    {
        // 只套用"内容相关"的属性；目录/重解析点这类绝不能套到文件上
        const DWORD allowed = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                              FILE_ATTRIBUTE_SYSTEM   | FILE_ATTRIBUTE_ARCHIVE;

        const DWORD wanted = info.attributes & allowed;

        if (wanted != 0)
        {
            ::SetFileAttributesW(longPath.c_str(), wanted);
        }
    }

    return true;
}

} // namespace shellclip
