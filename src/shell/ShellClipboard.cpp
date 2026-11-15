// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ShellClipboard.cpp
// ============================================================================

#include "ShellClipboard.h"

#include <shlobj.h>     // DROPFILES、DROPEFFECT_*
#include <shellapi.h>   // DragQueryFileW

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
    return ::IsClipboardFormatAvailable(CF_HDROP) != FALSE;
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

} // namespace shellclip
