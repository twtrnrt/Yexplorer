// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ShellDragDrop.cpp
//
// 拖放实现（详见头文件说明）。
// ============================================================================

#include "ShellDragDrop.h"

#include "core/Logger.h"

#include <commoncontrols.h>
#include <cstring>      // std::memcpy（往 DROPDESCRIPTION 结构里拷数据）
#include <map>
#include <memory>
#include <shellapi.h>
#include <shlobj.h>
#include <wil/com.h>

namespace shell
{

// ============================================================================
// 小工具
// ============================================================================

// 把路径列表打包成 CF_HDROP 需要的内存块（DROPFILES 头 + 一串宽字符路径）
static HGLOBAL BuildDropFilesHandle(const std::vector<std::wstring>& paths)
{
    // 字符数 = 每条路径 + 结尾的 \0，再额外补一个 \0
    size_t totalChars = 1;
    for (const auto& path : paths)
    {
        totalChars += path.size() + 1;
    }

    size_t totalBytes = sizeof(DROPFILES) + totalChars * sizeof(wchar_t);

    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalBytes);
    if (memory == nullptr)
    {
        return nullptr;
    }

    void* raw = ::GlobalLock(memory);
    if (raw == nullptr)
    {
        ::GlobalFree(memory);
        return nullptr;
    }

    DROPFILES* dropFiles = static_cast<DROPFILES*>(raw);
    dropFiles->pFiles    = sizeof(DROPFILES);
    dropFiles->fWide     = TRUE;    // 用宽字符路径（中文路径才不会乱码）

    wchar_t* cursor = reinterpret_cast<wchar_t*>(
        reinterpret_cast<BYTE*>(raw) + sizeof(DROPFILES));

    for (const auto& path : paths)
    {
        ::memcpy(cursor, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
        cursor += path.size() + 1;
    }
    *cursor = L'\0';

    ::GlobalUnlock(memory);
    return memory;
}

// "Preferred DropEffect" 的剪贴板格式 id（延迟注册一次）
static UINT PreferredDropEffectFormat()
{
    static UINT formatId = ::RegisterClipboardFormatW(L"Preferred DropEffect");
    return formatId;
}

// ============================================================================
// 拖动源提供的数据对象
//
// 只需要支持两种格式就够用：
//   CF_HDROP           —— 文件路径列表（绝大多数程序要的就是它）
//   Preferred DropEffect —— 声明的首选效果（复制 / 移动）
// ============================================================================
class SimpleDataObject final : public IDataObject
{
public:
    SimpleDataObject(std::vector<std::wstring> paths, DWORD preferredEffect)
        : m_paths(std::move(paths))
        , m_preferredEffect(preferredEffect)
    {
    }

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr)
        {
            return E_POINTER;
        }

        if (riid == IID_IUnknown || riid == IID_IDataObject)
        {
            *ppv = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }

        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(::InterlockedIncrement(&m_refCount));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG remaining = static_cast<ULONG>(::InterlockedDecrement(&m_refCount));
        if (remaining == 0)
        {
            delete this;
        }
        return remaining;
    }

    // ---- IDataObject ----
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override
    {
        if (format == nullptr || medium == nullptr)
        {
            return E_INVALIDARG;
        }

        ::ZeroMemory(medium, sizeof(*medium));

        // 文件路径列表
        if (format->cfFormat == CF_HDROP && (format->tymed & TYMED_HGLOBAL) != 0)
        {
            HGLOBAL handle = BuildDropFilesHandle(m_paths);
            if (handle == nullptr)
            {
                return E_OUTOFMEMORY;
            }

            medium->tymed          = TYMED_HGLOBAL;
            medium->hGlobal        = handle;
            medium->pUnkForRelease = nullptr;
            return S_OK;
        }

        // 首选效果（一个 DWORD）
        // 只有在明确要声明时才提供：声明 COPY|MOVE 会让资源管理器把默认效果判成"复制"，
        // 结果它只改光标、不做实际文件操作。留空就让它按自己的规则（同盘移动 / 跨盘复制 / Ctrl 复制）判断。
        UINT effectFormat = PreferredDropEffectFormat();
        if (m_preferredEffect != 0 && effectFormat != 0 && format->cfFormat == effectFormat &&
            (format->tymed & TYMED_HGLOBAL) != 0)
        {
            HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
            if (handle == nullptr)
            {
                return E_OUTOFMEMORY;
            }

            void* raw = ::GlobalLock(handle);
            if (raw == nullptr)
            {
                ::GlobalFree(handle);
                return E_OUTOFMEMORY;
            }

            *static_cast<DWORD*>(raw) = m_preferredEffect;
            ::GlobalUnlock(handle);

            medium->tymed          = TYMED_HGLOBAL;
            medium->hGlobal        = handle;
            medium->pUnkForRelease = nullptr;
            return S_OK;
        }

        // ---- 之前收下的其它格式：拖拽助手来读 DROPDESCRIPTION 走这里 ----
        ExtraFormat* extra = FindExtra(format);
        if (extra != nullptr)
        {
            HGLOBAL copy = DuplicateGlobal(extra->data);
            if (copy == nullptr)
            {
                return E_OUTOFMEMORY;
            }

            medium->tymed          = TYMED_HGLOBAL;
            medium->hGlobal        = copy;
            medium->pUnkForRelease = nullptr;
            return S_OK;
        }

        return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
    {
        if (format == nullptr)
        {
            return E_INVALIDARG;
        }

        if (format->cfFormat == CF_HDROP && (format->tymed & TYMED_HGLOBAL) != 0)
        {
            return S_OK;
        }

        UINT effectFormat = PreferredDropEffectFormat();
        if (m_preferredEffect != 0 && effectFormat != 0 && format->cfFormat == effectFormat && (format->tymed & TYMED_HGLOBAL) != 0)
        {
            return S_OK;
        }

        // 之前收下的其它格式
        if (FindExtra(format) != nullptr)
        {
            return S_OK;
        }

        return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* output) override
    {
        if (output != nullptr)
        {
            output->ptd = nullptr;
        }
        return DATA_S_SAMEFORMATETC;
    }

    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release) override
    {
        // 放置目标在放下之后，常会把"最终采用的效果"回写给拖动源（资源管理器就会这么做）。
        // 以前这里直接返回 E_NOTIMPL，目标会认为源不支持回写，有可能放弃整个文件操作。
        if (format == nullptr || medium == nullptr)
        {
            return E_INVALIDARG;
        }

        UINT effectFormat = PreferredDropEffectFormat();
        if (effectFormat != 0 && format->cfFormat == effectFormat &&
            medium->tymed == TYMED_HGLOBAL && medium->hGlobal != nullptr)
        {
            void* raw = ::GlobalLock(medium->hGlobal);
            if (raw != nullptr)
            {
                m_preferredEffect = *static_cast<DWORD*>(raw);
                ::GlobalUnlock(medium->hGlobal);
            }

            if (release != FALSE)
            {
                ::ReleaseStgMedium(medium);
            }
            return S_OK;
        }

        // ---- 其它格式：收下并保存 ----
        //
        // 光标右下角那行"复制到 xxx"就是这么来的：放置目标把 DROPDESCRIPTION 写进
        // 拖动源的数据对象，系统拖拽助手再读出来画。源这边必须肯收、还能读回。
        if (medium->tymed != TYMED_HGLOBAL || medium->hGlobal == nullptr)
        {
            return E_NOTIMPL;
        }

        HGLOBAL copy = DuplicateGlobal(medium->hGlobal);

        if (release != FALSE)
        {
            ::ReleaseStgMedium(medium);   // 传了 fRelease，源这份由我们负责释放
        }

        if (copy == nullptr)
        {
            return E_OUTOFMEMORY;
        }

        // 同一格式重复设置：替换旧的
        ExtraFormat* existing = FindExtra(format);
        if (existing != nullptr)
        {
            ::GlobalFree(existing->data);
            existing->data = copy;
            return S_OK;
        }

        ExtraFormat extra;
        extra.format        = *format;
        extra.format.ptd    = nullptr;   // 格式里的目标设备指针不能留着
        extra.format.lindex = -1;
        extra.data          = copy;
        m_extraFormats.push_back(extra);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** enumFormat) override
    {
        if (enumFormat == nullptr)
        {
            return E_POINTER;
        }

        // 能写回来的格式：首选效果 + 光标右下角那行提示字（DROPDESCRIPTION）
        if (direction == DATADIR_SET)
        {
            FORMATETC setFormats[2] = {};

            setFormats[0].cfFormat = static_cast<CLIPFORMAT>(PreferredDropEffectFormat());

            if (setFormats[0].cfFormat == 0)
            {
                setFormats[0].cfFormat = static_cast<CLIPFORMAT>(
                    ::RegisterClipboardFormatW(CFSTR_DROPDESCRIPTION));
            }

            for (auto& format : setFormats)
            {
                format.dwAspect = DVASPECT_CONTENT;
                format.lindex   = -1;
                format.tymed    = TYMED_HGLOBAL;
            }

            setFormats[1].cfFormat = static_cast<CLIPFORMAT>(
                ::RegisterClipboardFormatW(CFSTR_DROPDESCRIPTION));

            return ::SHCreateStdEnumFmtEtc(2, setFormats, enumFormat);
        }

        if (direction != DATADIR_GET)
        {
            *enumFormat = nullptr;
            return E_NOTIMPL;
        }

        // 能取出去的格式：文件列表 + 首选效果 + 目标塞进来的其它格式
        std::vector<FORMATETC> formats;

        FORMATETC fileDrop = {};
        fileDrop.cfFormat = static_cast<CLIPFORMAT>(CF_HDROP);
        fileDrop.dwAspect = DVASPECT_CONTENT;
        fileDrop.lindex   = -1;
        fileDrop.tymed    = TYMED_HGLOBAL;
        formats.push_back(fileDrop);

        if (m_preferredEffect != 0)
        {
            FORMATETC effect = fileDrop;
            effect.cfFormat = static_cast<CLIPFORMAT>(PreferredDropEffectFormat());
            formats.push_back(effect);
        }

        for (const auto& extra : m_extraFormats)
        {
            formats.push_back(extra.format);
        }

        return ::SHCreateStdEnumFmtEtc(static_cast<UINT>(formats.size()), formats.data(), enumFormat);
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    // 一份"别的程序塞进来"的格式数据
    // 最要紧的是 DROPDESCRIPTION（光标右下角"复制到 xxx"那行字）：
    //   放置目标把它写进拖动源的数据对象，系统拖拽助手再从数据对象读出来画。
    //   以前这里只认「首选效果」一种格式、其它一律 E_NOTIMPL，
    //   结果就是资源管理器拖我们程序的文件时不显示那行字，我们拖进来时也显示不了。
    struct ExtraFormat
    {
        FORMATETC format = {};
        HGLOBAL   data   = nullptr;
    };

    // 把一坨 HGLOBAL 复制一份（同一格式可能被反复读写，各管各的生命周期最省心）
    static HGLOBAL DuplicateGlobal(HGLOBAL source)
    {
        if (source == nullptr)
        {
            return nullptr;
        }

        const SIZE_T size = ::GlobalSize(source);
        if (size == 0)
        {
            return nullptr;
        }

        HGLOBAL copy = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
        if (copy == nullptr)
        {
            return nullptr;
        }

        void* sourceRaw = ::GlobalLock(source);
        void* copyRaw   = ::GlobalLock(copy);

        if (sourceRaw == nullptr || copyRaw == nullptr)
        {
            if (sourceRaw != nullptr)
            {
                ::GlobalUnlock(source);
            }
            if (copyRaw != nullptr)
            {
                ::GlobalUnlock(copy);
            }
            ::GlobalFree(copy);
            return nullptr;
        }

        std::memcpy(copyRaw, sourceRaw, size);
        ::GlobalUnlock(source);
        ::GlobalUnlock(copy);
        return copy;
    }

    // 找一个已经收下的格式（没有就返回 nullptr）
    ExtraFormat* FindExtra(const FORMATETC* format)
    {
        if (format == nullptr)
        {
            return nullptr;
        }

        for (auto& extra : m_extraFormats)
        {
            if (extra.format.cfFormat == format->cfFormat && extra.data != nullptr)
            {
                return &extra;
            }
        }

        return nullptr;
    }

    LONG                      m_refCount = 1;
    std::vector<std::wstring> m_paths;
    DWORD                     m_preferredEffect;

    std::vector<ExtraFormat>  m_extraFormats;

public:
    ~SimpleDataObject()
    {
        for (auto& extra : m_extraFormats)
        {
            if (extra.data != nullptr)
            {
                ::GlobalFree(extra.data);
                extra.data = nullptr;
            }
        }
    }
};

// ============================================================================
// 拖动源：决定「什么时候算放下、什么算取消」，以及鼠标指针样式
// ============================================================================
class SimpleDropSource final : public IDropSource
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr)
        {
            return E_POINTER;
        }

        if (riid == IID_IUnknown || riid == IID_IDropSource)
        {
            *ppv = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }

        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(::InterlockedIncrement(&m_refCount));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG remaining = static_cast<ULONG>(::InterlockedDecrement(&m_refCount));
        if (remaining == 0)
        {
            delete this;
        }
        return remaining;
    }

    // 按下 Esc 取消；左键松开就算放下
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override
    {
        if (escapePressed)
        {
            return DRAGDROP_S_CANCEL;
        }

        if ((keyState & MK_LBUTTON) == 0)
        {
            return DRAGDROP_S_DROP;
        }

        return S_OK;
    }

    // 用系统自带的光标（复制带加号、移动不带），和资源管理器一样
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override
    {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    LONG m_refCount = 1;
};

// ============================================================================
// 效果计算
// ============================================================================

DWORD ComputeDropEffect(DWORD keyState, DWORD sourcePreferredEffect, bool sameDrive)
{
    // 修饰键优先，和资源管理器一致
    if ((keyState & MK_CONTROL) != 0)
    {
        return DROPEFFECT_COPY;
    }
    if ((keyState & MK_SHIFT) != 0)
    {
        return DROPEFFECT_MOVE;
    }
    if ((keyState & MK_ALT) != 0)
    {
        return DROPEFFECT_LINK;    // 建快捷方式
    }

    // 什么都没按：同盘移动、跨盘复制
    if (!sameDrive)
    {
        return DROPEFFECT_COPY;
    }

    // 拖动源明确说了"只复制"（比如从压缩包里往外拖），那就复制
    if ((sourcePreferredEffect & DROPEFFECT_COPY) != 0 &&
        (sourcePreferredEffect & DROPEFFECT_MOVE) == 0)
    {
        return DROPEFFECT_COPY;
    }

    return DROPEFFECT_MOVE;
}

bool IsSameDrive(const std::wstring& pathA, const std::wstring& pathB)
{
    if (pathA.empty() || pathB.empty())
    {
        return false;
    }

    wchar_t rootA[MAX_PATH] = {};
    wchar_t rootB[MAX_PATH] = {};

    // 本地盘拿 "C:\"；网络路径拿 "\\服务器\共享\"
    if (::GetVolumePathNameW(pathA.c_str(), rootA, MAX_PATH) &&
        ::GetVolumePathNameW(pathB.c_str(), rootB, MAX_PATH))
    {
        return ::_wcsicmp(rootA, rootB) == 0;
    }

    // 退路：比较前两个字符（C: vs D:）
    return ::_wcsnicmp(pathA.c_str(), pathB.c_str(), 2) == 0;
}

bool IsSelfOrDescendantPath(const std::wstring& candidate, const std::wstring& ancestor)
{
    if (candidate.empty() || ancestor.empty())
    {
        return false;
    }

    // 统一去掉末尾的分隔符：否则 "C:\A\" 和 "C:\A" 会被当成两个不同的路径
    auto trimmed = [](const std::wstring& path) -> std::wstring
    {
        std::wstring result = path;
        while (!result.empty() && (result.back() == L'\\' || result.back() == L'/'))
        {
            result.pop_back();
        }
        return result;
    };

    const std::wstring candidatePath = trimmed(candidate);
    const std::wstring ancestorPath  = trimmed(ancestor);

    if (candidatePath.empty() || ancestorPath.empty())
    {
        return false;
    }

    // ① 就是它自己：拖文件夹放到它自己身上
    if (::_wcsicmp(candidatePath.c_str(), ancestorPath.c_str()) == 0)
    {
        return true;
    }

    // ② 在它里面：祖先路径再加一个反斜杠，必须是候选路径的开头
    //    （加反斜杠是为了避免 "C:\ABB" 被误判成在 "C:\AB" 里面）
    std::wstring prefix = ancestorPath;
    prefix.push_back(L'\\');

    return ::_wcsnicmp(candidatePath.c_str(), prefix.c_str(), prefix.size()) == 0;
}

// ============================================================================
// 拖出
// ============================================================================

namespace
{
// 拖放结束后，放置目标（资源管理器等）有时会在后台继续读数据，
// 这时候如果我们立刻把 IDataObject 销毁，它就读不到了（表现为"拖放成功但文件没落地"）。
// 所以这里保留最近几次的数据对象，隔几轮再释放。
std::vector<wil::com_ptr<IDataObject>> g_recentDataObjects;
const size_t                           kKeepRecentDataObjects = 4;

// ----------------------------------------------------------------------------
// 标准壳数据对象
//
// 资源管理器自己往外拖时用的就是 SHCreateDataObject 造出来的对象。
// 除了 CF_HDROP，它还带 "Shell IDList Array"（ID 列表）等格式，
// 有些目标（含资源管理器的部分路径）只认 ID 列表，光给 CF_HDROP 会出现
// "拖放成功但什么都没发生"。所以优先用它，造不出来才退回我们自己的简单对象。
//
// 这些 PIDL 由我们自己申请，必须活到数据对象销毁，所以用一个结构体一起持有。
// ----------------------------------------------------------------------------
struct ShellDataObjectHolder
{
    std::vector<PIDLIST_ABSOLUTE> pidls;      // 申请过的所有绝对 PIDL（析构时统一释放）
    wil::com_ptr<IDataObject>     object;     // 造出来的数据对象

    ~ShellDataObjectHolder()
    {
        for (auto pidl : pidls)
        {
            if (pidl != nullptr)
            {
                ::ILFree(pidl);
            }
        }
    }
};

// 最近几次的标准数据对象，和上面一样延迟释放
std::vector<std::shared_ptr<ShellDataObjectHolder>> g_recentShellObjects;

// 造标准数据对象。要求所有文件在同一个父目录下（跨目录多选时返回空，让调用方退回简单对象）。
std::shared_ptr<ShellDataObjectHolder> CreateShellDataObject(const std::vector<std::wstring>& paths)
{
    if (paths.empty())
    {
        return nullptr;
    }

    auto holder = std::make_shared<ShellDataObjectHolder>();

    PIDLIST_ABSOLUTE              parentPidl = nullptr;   // 所有项的公共父目录
    std::vector<PIDLIST_RELATIVE> children;               // 各项相对父目录的 PIDL

    for (const auto& path : paths)
    {
        PIDLIST_ABSOLUTE full = ::ILCreateFromPathW(path.c_str());
        if (full == nullptr)
        {
            return nullptr;
        }
        holder->pidls.push_back(full);

        PIDLIST_ABSOLUTE parent = ::ILClone(full);
        if (parent == nullptr)
        {
            return nullptr;
        }
        ::ILRemoveLastID(parent);

        if (parentPidl == nullptr)
        {
            parentPidl = parent;
            holder->pidls.push_back(parent);
        }
        else
        {
            const bool sameParent = (::ILIsEqual(parentPidl, parent) != FALSE);
            ::ILFree(parent);
            if (!sameParent)
            {
                return nullptr;   // 不在同一目录，退回简单对象
            }
        }

        children.push_back(::ILFindLastID(full));
    }

    if (parentPidl == nullptr || children.empty())
    {
        return nullptr;
    }

    // SHCreateDataObject 要的是"每一项相对父目录的 PIDL"数组
    std::vector<PCUITEMID_CHILD> childPtrs;
    childPtrs.reserve(children.size());
    for (auto child : children)
    {
        childPtrs.push_back(child);
    }

    wil::com_ptr<IDataObject> object;
    HRESULT hr = ::SHCreateDataObject(parentPidl,
                                      static_cast<UINT>(childPtrs.size()),
                                      childPtrs.data(),
                                      nullptr,
                                      IID_PPV_ARGS(&object));
    if (FAILED(hr) || object == nullptr)
    {
        return nullptr;
    }

    holder->object = std::move(object);
    return holder;
}
}

DWORD DoDragFiles(const std::vector<std::wstring>& paths,
                  HWND                            sourceWindow,
                  POINT                           dragPointClient,
                  DWORD                           allowedEffects,
                  DWORD                           preferredEffect)
{
    if (paths.empty())
    {
        return DROPEFFECT_NONE;
    }

    // 优先用标准壳数据对象（和资源管理器一致），造不出来才用我们自己的简单对象
    std::shared_ptr<ShellDataObjectHolder> shellHolder = CreateShellDataObject(paths);

    wil::com_ptr<IDataObject> dataObject;
    if (shellHolder != nullptr)
    {
        dataObject = shellHolder->object;
    }
    else
    {
        dataObject.attach(new SimpleDataObject(paths, preferredEffect));
    }
    if (dataObject == nullptr)
    {
        return DROPEFFECT_NONE;
    }

    // 标准壳对象自己不带"首选效果"，补一份写进去：接收方据此知道只能复制。
    // 写失败不影响拖动（接收方会按同盘/跨盘自己判断），只是语义没那么精确。
    if (preferredEffect != 0 && shellHolder != nullptr)
    {
        STGMEDIUM medium = {};
        medium.tymed     = TYMED_HGLOBAL;
        medium.hGlobal   = ::GlobalAlloc(GHND, sizeof(DWORD));

        if (medium.hGlobal != nullptr)
        {
            void* raw = ::GlobalLock(medium.hGlobal);
            if (raw != nullptr)
            {
                *static_cast<DWORD*>(raw) = preferredEffect;
                ::GlobalUnlock(medium.hGlobal);

                FORMATETC format = {};
                format.cfFormat = static_cast<CLIPFORMAT>(
                    ::RegisterClipboardFormatW(L"Preferred DropEffect"));
                format.dwAspect = DVASPECT_CONTENT;
                format.lindex   = -1;
                format.tymed    = TYMED_HGLOBAL;

                dataObject->SetData(&format, &medium, TRUE);
            }
            else
            {
                ::GlobalFree(medium.hGlobal);
            }
        }
    }

    Logger::Info(shellHolder != nullptr
                     ? L"拖出：使用标准壳数据对象（带 ID 列表，兼容性最好）"
                     : L"拖出：使用简易数据对象（只有 CF_HDROP）");

    wil::com_ptr<IDropSource> dropSource;
    dropSource.attach(new SimpleDropSource());
    if (dropSource == nullptr)
    {
        return DROPEFFECT_NONE;
    }

    // 让系统帮我们做一个"跟着鼠标走的半透明小图"（拖动时更接近资源管理器的观感）。
    // 窗口没实现 DI_GETDRAGIMAGE 时这一步会失败，那就退化成只有光标，不影响拖动本身。
    wil::com_ptr<IDragSourceHelper> dragHelper;
    if (SUCCEEDED(::CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&dragHelper))))
    {
        POINT startPoint = dragPointClient;
        dragHelper->InitializeFromWindow(sourceWindow, &startPoint, dataObject.get());
    }

    DWORD finalEffect = DROPEFFECT_NONE;

    HRESULT hr = ::DoDragDrop(dataObject.get(), dropSource.get(),
                             (DWORD)allowedEffects, &finalEffect);

    if (hr == DRAGDROP_S_CANCEL)
    {
        finalEffect = DROPEFFECT_NONE;
    }

    Logger::Info(L"拖出结束：项数=" + std::to_wstring(paths.size()) +
                 L"，结果=" + Logger::FormatHresult(static_cast<long>(hr)) +
                 L"，效果=" + std::to_wstring(finalEffect));

    // 拖完先别销毁数据对象：目标程序可能还在后台读（见上面 g_recentDataObjects 的说明）
    if (shellHolder != nullptr)
    {
        g_recentShellObjects.push_back(shellHolder);
        while (g_recentShellObjects.size() > kKeepRecentDataObjects)
        {
            g_recentShellObjects.erase(g_recentShellObjects.begin());
        }
    }
    else
    {
        g_recentDataObjects.push_back(dataObject);
        while (g_recentDataObjects.size() > kKeepRecentDataObjects)
        {
            g_recentDataObjects.erase(g_recentDataObjects.begin());
        }
    }

    return finalEffect;
}

// ============================================================================
// 放置目标
// ============================================================================
namespace
{
std::map<HWND, IDropTarget*> g_dropTargets;   // HWND → 放置目标（持有引用）

// ----------------------------------------------------------------------------
// 拖拽提示条（光标右下方那条「复制到 xxx」的小字）
//
// 为什么自己画：实测系统给的 IDropTargetHelper 根本不去读数据对象里的
// DROPDESCRIPTION，资源管理器显示的那行字是它自己画的。
// 所以这里也自己画一条，长相照着系统来：白底、圆角、浅灰描边、深灰小字。
// 窗口属性：不吃鼠标（WS_EX_TRANSPARENT）、不抢焦点（WS_EX_NOACTIVATE）、置顶。
// ----------------------------------------------------------------------------
constexpr int kDropTipPaddingX = 10;   // 左右留白（提示条高度固定 30，宽度按内容算，不需要上下留白）
constexpr int kDropTipOffsetX  = 16;   // 相对光标的偏移（系统差不多也是这个位置）
constexpr int kDropTipOffsetY  = 22;

// ---- 配色（用户定的）----
constexpr COLORREF kDropTipBackground = RGB(0xF9, 0xF9, 0xF9);   // 底色
constexpr COLORREF kDropTipBorder     = RGB(0xED, 0xED, 0xED);   // 边框
constexpr COLORREF kDropTipAction     = RGB(0x00, 0x33, 0xCC);   // 「复制到 / 移动到 / 添加到」
constexpr COLORREF kDropTipTarget     = RGB(0x00, 0x00, 0x66);   // 目标文件夹名
constexpr COLORREF kDropTipForbidden  = RGB(0xCC, 0x00, 0x00);   // 「不能放到此处」

// ---- 符号（自己画，不依赖字体里有没有这个字符）----
constexpr int kDropTipSymbolGap  = 3;    // 符号和文字之间留的缝
constexpr int kDropTipWordGap    = 4;    // 动作词和目标名之间留的缝
constexpr int kDropTipMaxTarget  = 320;  // 目标名最长画这么宽，再长就省略号

// 提示条固定高度（用户定的：固定 30，不跟着符号大小变，三种状态一样高）。
constexpr int kDropTipHeight          = 30;
constexpr int kDropTipSymbolSizeDot   = 12;   // + 号、禁止圈的方框（16 → 14 → 13 → 12）
constexpr int kDropTipSymbolSizeArrow = 16;   // 箭头的方框（原 18，缩到 90%）
constexpr int kDropTipSymbolShiftY    = -1;   // + 号和禁止圈的垂直偏移（负数 = 上移）；箭头不受影响
constexpr int kDropTipSupersample     = 4;    // 超采样倍数：4 倍画、再缩回去 = 抗锯齿

// 提示条上要显示的内容：符号 + 动作词 + 目标名，三种颜色分开画
struct DropTipContent
{
    // 0 = 不画符号，1 = 加号（复制 / 添加），2 = 箭头（移动 / 链接），3 = 红圈斜杠（禁止）
    int          symbol = 0;

    std::wstring actionText;      // 「复制到」「移动到」「链接到」「添加到」
    std::wstring targetText;      // 目标文件夹名（禁止时是「此处」）
    bool         forbidden = false;   // 禁止样式：符号和字都用红

    // 给"内容没变就别重画"用的签名
    std::wstring Signature() const
    {
        return actionText + L"|" + targetText + L"|" +
               std::to_wstring(symbol) + (forbidden ? L"|F" : L"");
    }
};

DropTipContent g_dropTipContent;       // 当前要显示的内容（同一时刻只有一个拖动，全局够用）
HFONT         g_dropTipFont = nullptr; // 系统界面字体（Win11 上是微软雅黑 UI）

// 系统界面字体，取一次就缓存
HFONT DropTipFont()
{
    if (g_dropTipFont == nullptr)
    {
        NONCLIENTMETRICSW metrics = {};
        metrics.cbSize = sizeof(metrics);

        if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
        {
            g_dropTipFont = ::CreateFontIndirectW(&metrics.lfMessageFont);
        }
    }

    return g_dropTipFont;
}

// ---- 符号图标：自己画，不再用字体里的字符 ----
//
// 为什么放弃字符字形：字体里每个符号的笔画位置都不一样（+ 号在数学轴上、⊘ 偏上、
// ⬉ 偏下），字号一放大，笔画中心跟文字中线就对不上；公式只能算 em 框，算不了笔画。
// 而且 ⬉ 在小字号下只有一两像素宽，细得像一道划痕。
//
// 现在的做法：几何自己定（笔画中心 = 图标框中心，跟文字中线天然对齐），
// 画的时候先在 4 倍尺寸上画，再缩回目标尺寸——缩放这一步本身就等于抗锯齿，
// 而 GDI 自己是不做抗锯齿的。

int DropTipSymbolSize(int symbol)
{
    // 箭头的笔画最细，给它大两像素看起来才够重
    return (symbol == 2) ? kDropTipSymbolSizeArrow : kDropTipSymbolSizeDot;
}

// 在 target 的 (left, top) 起、size×size 的方框里画符号
//
// 实现分三步：
//   1. 建一张"size×4"的黑底白线位图，把图形按 4 倍尺寸画上去（GDI 画的是硬边）；
//   2. 每 4×4 个像素求平均，得到 0~255 的覆盖率（= 这个像素被图形盖住了多少）；
//   3. 用覆盖率把前景色跟提示条底色混一遍，写进 1 倍位图，再 BitBlt 贴到 target。
//
// 因为提示条底色是固定的 F9F9F9，混合后像素完全不透明，所以不需要 AlphaBlend，
// 也不会有黑边（黑边就是"透明像素跟黑底混"混出来的）。
void DrawDropTipSymbol(HDC target, int symbol, int left, int top, int size, COLORREF color)
{
    if (target == nullptr || symbol < 1 || symbol > 3 || size <= 0)
    {
        return;
    }

    const int supersample = kDropTipSupersample;
    const int big         = size * supersample;

    // ---- 第一步：4 倍大小的画布（32 位；biHeight 取负数表示上下方向跟屏幕一致）----
    BITMAPINFO canvasInfo = {};
    canvasInfo.bmiHeader.biSize        = sizeof(canvasInfo.bmiHeader);
    canvasInfo.bmiHeader.biWidth       = big;
    canvasInfo.bmiHeader.biHeight      = -big;
    canvasInfo.bmiHeader.biPlanes      = 1;
    canvasInfo.bmiHeader.biBitCount    = 32;
    canvasInfo.bmiHeader.biCompression = BI_RGB;

    void*   canvasPixels = nullptr;
    HDC     canvasDc     = ::CreateCompatibleDC(target);
    HBITMAP canvasBitmap = ::CreateDIBSection(target, &canvasInfo, DIB_RGB_COLORS,
                                              &canvasPixels, nullptr, 0);

    if (canvasDc == nullptr || canvasBitmap == nullptr || canvasPixels == nullptr)
    {
        if (canvasBitmap != nullptr)
        {
            ::DeleteObject(canvasBitmap);
        }
        if (canvasDc != nullptr)
        {
            ::DeleteDC(canvasDc);
        }
        return;
    }

    HGDIOBJ oldCanvasBitmap = ::SelectObject(canvasDc, canvasBitmap);

    // 黑底 = 覆盖率 0，白线 = 覆盖率 255
    RECT canvasRect = { 0, 0, big, big };
    ::FillRect(canvasDc, &canvasRect, static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)));
    ::SetBkMode(canvasDc, TRANSPARENT);

    // 线宽：默认按目标尺寸的 13% 取（13 像素 → 2 像素）；
    // 箭头取 12%（16 像素 → 2 像素）——用户要求"柄粗一点"，
    // 同时头缩到 3.5 像素左右，两者粗细接近才不会糊成一根斜杠。
    const int strokePercent = (symbol == 2) ? 12 : 13;
    const int stroke    = (size * strokePercent + 50) / 100;
    const int penWidth  = ((stroke < 1) ? 1 : stroke) * supersample;

    HPEN   whitePen = ::CreatePen(PS_SOLID, penWidth, RGB(255, 255, 255));
    HGDIOBJ oldPen  = ::SelectObject(canvasDc, whitePen);
    ::SelectObject(canvasDc, ::GetStockObject(NULL_BRUSH));

    // 坐标一律写成"占比 × 大画布尺寸"，改图标尺寸不用改任何数字
    const int center = big / 2;

    switch (symbol)
    {
    case 1:   // 加号：横竖等长、等粗，正中对齐（原来那个字体字形的十字四边不等长）
    {
        const int arm = big * 40 / 100;   // 从中心往两边各伸 40%，总共占 80%
        ::MoveToEx(canvasDc, center - arm, center, nullptr);
        ::LineTo(canvasDc, center + arm, center);
        ::MoveToEx(canvasDc, center, center - arm, nullptr);
        ::LineTo(canvasDc, center, center + arm);
        break;
    }
    case 2:   // 箭头：一根 45 度斜杆 + 一个实心三角尖，指向「左上」
              // （原来是字体字符 ⬉ U+2B09 = NORTH WEST ARROW，也就是左上；之前画成左下是画错了方向）
    {
        // 斜杆从右下往左上；末端接到三角尖斜边的中点（0.29, 0.29），接缝才连得上
        ::MoveToEx(canvasDc, big * 78 / 100, big * 78 / 100, nullptr);
        ::LineTo(canvasDc, big * 30 / 100, big * 30 / 100);

        // 三角尖：尖在左上，另两点在它正下方、正右方。
        // 跨度 0.22（原来 0.32 → 0.26），用户说头还是太大，再收一圈。
        POINT triangle[3] = {
            { big * 18 / 100, big * 18 / 100 },   // 尖
            { big * 18 / 100, big * 40 / 100 },   // 下角
            { big * 40 / 100, big * 18 / 100 }    // 右角
        };
        ::SelectObject(canvasDc, ::GetStockObject(WHITE_BRUSH));
        ::Polygon(canvasDc, triangle, 3);
        break;
    }
    case 3:   // 禁止：圆环 + 一条 45 度斜杠，就是 ⊘ 的样子
    {
        ::Ellipse(canvasDc, big * 8 / 100, big * 8 / 100, big * 92 / 100, big * 92 / 100);
        // 斜杠要顶到圆环内壁上才像"禁止"（短一截会显得两根东西各在各的）
        ::MoveToEx(canvasDc, big * 20 / 100, big * 20 / 100, nullptr);
        ::LineTo(canvasDc, big * 80 / 100, big * 80 / 100);
        break;
    }
    default:
        break;
    }

    ::SelectObject(canvasDc, oldPen);
    ::DeleteObject(whitePen);

    // ---- 第二步 + 第三步：缩回 1 倍，边缩边跟底色混合 ----
    BITMAPINFO smallInfo = canvasInfo;
    smallInfo.bmiHeader.biWidth  = size;
    smallInfo.bmiHeader.biHeight = -size;

    void*   smallPixels = nullptr;
    HDC     smallDc     = ::CreateCompatibleDC(target);
    HBITMAP smallBitmap = ::CreateDIBSection(target, &smallInfo, DIB_RGB_COLORS,
                                             &smallPixels, nullptr, 0);

    if (smallDc != nullptr && smallBitmap != nullptr && smallPixels != nullptr)
    {
        HGDIOBJ oldSmallBitmap = ::SelectObject(smallDc, smallBitmap);

        // GDI 是延迟绘制的，先冲一下，保证画的东西已经落到 DIB 内存里
        ::GdiFlush();

        const BYTE* source = static_cast<const BYTE*>(canvasPixels);   // 每像素 BGRA
        BYTE*       dest   = static_cast<BYTE*>(smallPixels);

        // COLORREF 的字节顺序是 0x00BBGGRR（红在最低字节），
        // 这里直接取字节而不是用 GetRValue/GetGValue 宏：那些宏对常量色值会触发截断警告
        const int backgroundR = static_cast<int>(kDropTipBackground & 0xFF);
        const int backgroundG = static_cast<int>((kDropTipBackground >> 8) & 0xFF);
        const int backgroundB = static_cast<int>((kDropTipBackground >> 16) & 0xFF);
        const int foregroundR = static_cast<int>(color & 0xFF);
        const int foregroundG = static_cast<int>((color >> 8) & 0xFF);
        const int foregroundB = static_cast<int>((color >> 16) & 0xFF);

        // 抗锯齿开关按符号分开：
        //   箭头（symbol 2）走硬边——用户实测硬边的箭头反而更利落；
        //   + 号和禁止圈走半透明过渡——圆环和细线用硬边会碎成台阶，用户说"不开没法看"。
        const bool smoothAllowed = (symbol != 2);

        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
            {
                // 4×4 块求平均 = 这个目标像素被图形覆盖了多少（0~255）
                int rawCoverage = 0;
                for (int sy = 0; sy < supersample; ++sy)
                {
                    const BYTE* row = source + static_cast<size_t>(y * supersample + sy) * big * 4;
                    for (int sx = 0; sx < supersample; ++sx)
                    {
                        rawCoverage += row[(x * supersample + sx) * 4 + 2];   // R 通道：白 255 / 黑 0
                    }
                }
                rawCoverage /= (supersample * supersample);

                // 硬边模式：覆盖率过半就算实色，其余全透明（笔画实心，但边缘有台阶）
                // 平滑模式：直接用覆盖率做半透明过渡（边缘柔和，不发虚的时候更耐看）
                const int coverage = smoothAllowed ? rawCoverage
                                                   : ((rawCoverage >= 128) ? 255 : 0);
                const int inverse  = 255 - coverage;
                const int offset   = (y * size + x) * 4;
                dest[offset + 0] = static_cast<BYTE>((backgroundB * inverse + foregroundB * coverage) / 255);
                dest[offset + 1] = static_cast<BYTE>((backgroundG * inverse + foregroundG * coverage) / 255);
                dest[offset + 2] = static_cast<BYTE>((backgroundR * inverse + foregroundR * coverage) / 255);
                dest[offset + 3] = 255;   // 混合过底色的，不透明
            }
        }

        ::BitBlt(target, left, top, size, size, smallDc, 0, 0, SRCCOPY);

        ::SelectObject(smallDc, oldSmallBitmap);
        ::DeleteObject(smallBitmap);
        ::DeleteDC(smallDc);
    }
    else
    {
        if (smallBitmap != nullptr)
        {
            ::DeleteObject(smallBitmap);
        }
        if (smallDc != nullptr)
        {
            ::DeleteDC(smallDc);
        }
    }

    // ---- 收尾：位图要先从 DC 里取回来才能删 ----
    ::SelectObject(canvasDc, oldCanvasBitmap);
    ::DeleteObject(canvasBitmap);
    ::DeleteDC(canvasDc);
}

LRESULT CALLBACK DropTipWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_ERASEBKGND:
        return 1;   // 背景自己画，别让系统刷一遍（会闪）

    case WM_PAINT:
    {
        PAINTSTRUCT paint = {};
        HDC dc = ::BeginPaint(hwnd, &paint);

        RECT client = {};
        ::GetClientRect(hwnd, &client);

        // 底色 + 描边（用户定的 F9F9F9 / EDEDED）
        HBRUSH background = ::CreateSolidBrush(kDropTipBackground);
        HPEN   border     = ::CreatePen(PS_SOLID, 1, kDropTipBorder);

        // 先把整块客户区刷上底色。为什么要铺满而不是只画一个圆角矩形：
        // 窗口区域（SetWindowRgn）的圆角是像素近似，跟 RoundRect 的圆角不完全重合，
        // 那几像素"没被画到"的地方会露出窗口底色（黑的），看起来就是边角发黑。
        ::FillRect(dc, &client, background);

        // 再只描边（里面已经有底色，不用再填一次）。
        // 描边往里收 1 像素：正好压在裁剪边界上的话会被切掉半条，圆角处留残点。
        ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
        ::SelectObject(dc, border);
        ::RoundRect(dc, client.left + 1, client.top + 1,
                    client.right - 1, client.bottom - 1, 14, 14);

        ::SetBkMode(dc, TRANSPARENT);

        HFONT font = DropTipFont();
        if (font != nullptr)
        {
            ::SelectObject(dc, font);
        }

        const DropTipContent& content = g_dropTipContent;

        // 分段量宽度：符号 + 动作词 + 目标名
        SIZE actionSize = {};
        if (!content.actionText.empty())
        {
            ::GetTextExtentPoint32W(dc, content.actionText.c_str(),
                                    static_cast<int>(content.actionText.size()), &actionSize);
        }

        SIZE targetSize = {};
        if (!content.targetText.empty())
        {
            ::GetTextExtentPoint32W(dc, content.targetText.c_str(),
                                    static_cast<int>(content.targetText.size()), &targetSize);

            // 目标名太长就按上限算，绘制时用省略号收尾
            if (targetSize.cx > kDropTipMaxTarget)
            {
                targetSize.cx = kDropTipMaxTarget;
            }
        }

        const bool hasSymbol  = (content.symbol != 0);
        const bool hasWordGap = (!content.actionText.empty() && !content.targetText.empty());

        // 符号宽度就是图标方框的宽度（符号是自己画的，跟字体无关）
        const int symbolWidth = hasSymbol ? DropTipSymbolSize(content.symbol) : 0;
        const int symbolGap   = hasSymbol ? kDropTipSymbolGap : 0;
        const int wordGap     = hasWordGap ? kDropTipWordGap : 0;

        const int totalWidth = symbolWidth + symbolGap + actionSize.cx + wordGap + targetSize.cx;

        int x = client.left + ((client.right - client.left) - totalWidth) / 2;
        if (x < client.left + kDropTipPaddingX)
        {
            x = client.left + kDropTipPaddingX;   // 窄窗口时靠左，别把符号顶出去
        }

        const COLORREF actionColor = content.forbidden ? kDropTipForbidden : kDropTipAction;
        const COLORREF targetColor = content.forbidden ? kDropTipForbidden : kDropTipTarget;

        // 符号：自己画的矢量图标（见 DrawDropTipSymbol）。
        //
        // 这里不再算基线了：图标是几何图形，方框中心就是它的视觉中心；
        // 方框在条子里垂直居中，文字也是按条子居中画的，两条中线天生重合。
        // （之前用字体字符时必须手算基线，是因为字体里每个符号的笔画都不在 em 框中心。）
        if (hasSymbol)
        {
            const int symbolSize = DropTipSymbolSize(content.symbol);

            // 垂直位置：默认居中。+ 号和禁止圈再往下 1 像素（用户定的），箭头不动。
            const int shift     = (content.symbol == 2) ? 0 : kDropTipSymbolShiftY;
            const int symbolTop = client.top + ((client.bottom - client.top) - symbolSize) / 2 + shift;

            DrawDropTipSymbol(dc, content.symbol, x, symbolTop, symbolSize, actionColor);

            x += symbolWidth + symbolGap;
        }

        // 动作词
        if (!content.actionText.empty())
        {
            RECT rect = { x, client.top, x + actionSize.cx + 2, client.bottom };
            ::SetTextColor(dc, actionColor);
            ::DrawTextW(dc, content.actionText.c_str(), static_cast<int>(content.actionText.size()),
                        &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            x += actionSize.cx + wordGap;
        }

        // 目标名
        if (!content.targetText.empty())
        {
            RECT rect = { x, client.top, x + targetSize.cx + 2, client.bottom };
            ::SetTextColor(dc, targetColor);
            ::DrawTextW(dc, content.targetText.c_str(), static_cast<int>(content.targetText.size()),
                        &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }

        ::DeleteObject(border);
        ::DeleteObject(background);

        ::EndPaint(hwnd, &paint);
        return 0;
    }
    }

    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

HWND CreateDropTipWindow()
{
    static bool s_classReady = false;

    if (!s_classReady)
    {
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize        = sizeof(windowClass);
        windowClass.style         = CS_SAVEBITS;   // 拖动中要反复挪位置，用它避免闪烁
        windowClass.lpfnWndProc   = DropTipWindowProc;
        windowClass.hInstance     = ::GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"YExplorerDropTip";

        ::RegisterClassExW(&windowClass);
        s_classReady = true;
    }

    HWND window = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
        L"YExplorerDropTip", L"",
        WS_POPUP,
        0, 0, 120, 30,
        nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);

    if (window == nullptr)
    {
        return nullptr;
    }

    // 稍微透明一点，不把下面的内容挡死（资源管理器那条其实也带一点透明）
    ::SetLayeredWindowAttributes(window, 0, 235, LWA_ALPHA);
    return window;
}
}

class WindowDropTarget final : public IDropTarget
{
public:
    WindowDropTarget(HWND hwnd, DropTargetCallbacks callbacks)
        : m_hwnd(hwnd)
        , m_callbacks(std::move(callbacks))
    {
    }

    ~WindowDropTarget()
    {
        HideDropTip();
    }

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr)
        {
            return E_POINTER;
        }

        if (riid == IID_IUnknown || riid == IID_IDropTarget)
        {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }

        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(::InterlockedIncrement(&m_refCount));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG remaining = static_cast<ULONG>(::InterlockedDecrement(&m_refCount));
        if (remaining == 0)
        {
            delete this;
        }
        return remaining;
    }

    // ---- IDropTarget ----
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keyState,
                                       POINTL pt, DWORD* effect) override
    {
        m_sourcePaths = ExtractPaths(data);
        m_sourcePreferredEffect = ExtractPreferredEffect(data);

        UpdateEffect(keyState, pt, effect);

        // 跟资源管理器一样，在光标右下方画一行「复制到 xxx」
        UpdateDropTip(pt, (effect != nullptr) ? *effect : DROPEFFECT_NONE);

        NotifyFeedback(pt);

        Logger::Info(L"拖入进入：源 " + std::to_wstring(m_sourcePaths.size()) +
                     L" 项，落点目录=" + ResolveDirectory(pt) +
                     L"，效果=" + std::to_wstring((effect != nullptr) ? *effect : 0));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD keyState, POINTL pt, DWORD* effect) override
    {
        UpdateEffect(keyState, pt, effect);
        UpdateDropTip(pt, (effect != nullptr) ? *effect : DROPEFFECT_NONE);

        NotifyFeedback(pt);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        HideDropTip();
        m_sourcePaths.clear();

        NotifyFeedback(POINTL{ -1, -1 }, false);   // 鼠标离开了：让宿主把高亮清掉
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keyState, POINTL pt, DWORD* effect) override
    {
        std::vector<std::wstring> sources = ExtractPaths(data);
        if (sources.empty())
        {
            sources = m_sourcePaths;
        }

        std::wstring targetDirectory = ResolveDirectory(pt);
        if (sources.empty() || targetDirectory.empty())
        {
            if (effect != nullptr)
            {
                *effect = DROPEFFECT_NONE;
            }

            HideDropTip();
            return S_OK;
        }

        DWORD chosen = ComputeDropEffect(keyState, ExtractPreferredEffect(data),
                                        IsSameDrive(sources.front(), targetDirectory));

        // 松手前再判一次（第二道保险）：落点是源自己、或者源里面的子目录时，
        // 复制和移动都会让系统报错并弹出它自己的错误框，所以这里直接按"无效落点"处理。
        // 建快捷方式（Alt）例外：在自己里面建一个指向自己的 .lnk 是合法的。
        if (chosen != DROPEFFECT_LINK && IsDropIntoSelf(targetDirectory, sources))
        {
            chosen = DROPEFFECT_NONE;
        }

        bool createShortcut = (chosen == DROPEFFECT_LINK);

        if (effect != nullptr)
        {
            *effect = chosen;
        }

        // 提示条先收掉，再真正落地（不然它会在屏幕上多留一会儿）
        HideDropTip();

        // 无效落点：什么都不执行。拖动阶段光标已经显示禁止了，这里是兜底，
        // 防止将来有别的入口（左栏树、地址栏）绕过拖动阶段的判定。
        if (chosen == DROPEFFECT_NONE)
        {
            Logger::Info(L"落点无效（目标是源自身或源的子目录），忽略");
            NotifyFeedback(POINTL{ -1, -1 }, false);
            m_sourcePaths.clear();
            return S_OK;
        }

        if (m_callbacks.PerformDrop)
        {
            m_callbacks.PerformDrop(sources, targetDirectory, chosen, createShortcut);
        }

        NotifyFeedback(POINTL{ -1, -1 }, false);   // 放完了：清掉文件夹高亮
        m_sourcePaths.clear();
        return S_OK;
    }

private:
    // ------------------------------------------------------------------
    // 光标右下角那行小字：「复制到 桌面」「移动到 文档」「链接到 xxx」
    // ------------------------------------------------------------------
    void UpdateDropTip(POINTL screenPoint, DWORD effect)
    {
        const std::wstring targetDirectory = ResolveDirectory(screenPoint);
        const DWORD effectMask = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;

        DropTipContent content;

        if (targetDirectory.empty() || (effect & effectMask) == 0)
        {
            // 这里放不了：跟系统一样提示一句（红字，也套在框里）
            content.forbidden   = true;
            content.symbol      = 3;
            content.actionText  = L"不能放到";
            content.targetText  = L"此处";
        }
        else
        {
            // 宿主有没有自己的一套说法？比如拖进左栏空白是「添加到 常用」，不是「复制到 某目录」
            bool custom = false;

            if (m_callbacks.ResolveDropHint)
            {
                std::wstring action;
                std::wstring name;

                if (m_callbacks.ResolveDropHint(POINT{ screenPoint.x, screenPoint.y }, action, name) &&
                    !action.empty())
                {
                    content.actionText = action;
                    content.targetText = name;
                    content.symbol     = 1;   // 自定义的通常是"添加"这类，用加号
                    custom             = true;
                }
            }

            if (!custom)
            {
                if ((effect & DROPEFFECT_LINK) != 0)
                {
                    content.actionText = L"链接到";
                    content.symbol     = 2;
                }
                else if ((effect & DROPEFFECT_MOVE) != 0)
                {
                    content.actionText = L"移动到";
                    content.symbol     = 2;
                }
                else
                {
                    content.actionText = L"复制到";
                    content.symbol     = 1;
                }

                content.targetText = ShellDisplayName(targetDirectory);
            }
        }

        // IDropTarget 回调给的 pt 本来就是屏幕坐标，直接拿来做锚点
        ShowDropTip(content, POINT{ screenPoint.x, screenPoint.y });
    }

    void ShowDropTip(const DropTipContent& content, POINT anchorPoint)
    {
        if (m_tipWindow == nullptr)
        {
            m_tipWindow = CreateDropTipWindow();
            if (m_tipWindow == nullptr)
            {
                return;
            }
        }

        const int targetX = anchorPoint.x + kDropTipOffsetX;
        const int targetY = anchorPoint.y + kDropTipOffsetY;

        // 内容没变、位置也基本没动就别折腾（DragOver 是高频回调，反复重画会闪）
        const int dx = (targetX > m_tipX) ? (targetX - m_tipX) : (m_tipX - targetX);
        const int dy = (targetY > m_tipY) ? (targetY - m_tipY) : (m_tipY - targetY);

        const std::wstring signature = content.Signature();

        if (signature == m_tipText && ::IsWindowVisible(m_tipWindow) != FALSE && dx < 3 && dy < 3)
        {
            return;
        }

        m_tipText = signature;
        m_tipX    = targetX;
        m_tipY    = targetY;

        g_dropTipContent = content;

        // 分段量宽度（跟 WM_PAINT 里的算法必须一致，否则字会被裁）
        HDC dc = ::GetDC(m_tipWindow);
        HFONT font = DropTipFont();
        HGDIOBJ previousFont = (font != nullptr) ? ::SelectObject(dc, font) : nullptr;

        SIZE actionSize = {};
        if (!content.actionText.empty())
        {
            ::GetTextExtentPoint32W(dc, content.actionText.c_str(),
                                    static_cast<int>(content.actionText.size()), &actionSize);
        }

        SIZE targetSize = {};
        if (!content.targetText.empty())
        {
            ::GetTextExtentPoint32W(dc, content.targetText.c_str(),
                                    static_cast<int>(content.targetText.size()), &targetSize);

            if (targetSize.cx > kDropTipMaxTarget)
            {
                targetSize.cx = kDropTipMaxTarget;
            }
        }

        if (previousFont != nullptr)
        {
            ::SelectObject(dc, previousFont);
        }
        ::ReleaseDC(m_tipWindow, dc);

        const bool hasSymbol  = (content.symbol != 0);
        const bool hasWordGap = (!content.actionText.empty() && !content.targetText.empty());

        // 符号宽度就是图标方框宽度（自己画的，跟字体无关）
        const int symbolWidth = hasSymbol ? DropTipSymbolSize(content.symbol) : 0;
        const int symbolGap   = hasSymbol ? kDropTipSymbolGap : 0;
        const int wordGap     = hasWordGap ? kDropTipWordGap : 0;

        const int width  = symbolWidth + symbolGap + actionSize.cx + wordGap + targetSize.cx +
                           kDropTipPaddingX * 2;

        // 高度固定 30 像素（用户定的）：三种状态（复制 / 移动 / 禁止）条子一样高，
        // 拖动时状态切换不会一跳一跳的；18 像素的箭头放进去上下各留 6，刚好。
        const int height = kDropTipHeight;

        ::SetWindowPos(m_tipWindow, HWND_TOPMOST, m_tipX, m_tipY, width, height,
                       SWP_NOACTIVATE | SWP_SHOWWINDOW);

        // 圆角裁剪：窗口形状本身就是圆角矩形。
        // 只靠 RoundRect 画白底的话，圆角外面那几块像素是"没画过"的窗口底色（黑的），
        // 看上去就是几个黑角；用区域裁掉之后，圆角外面什么都不存在，边缘就干净了。
        // 30 像素高配 12 的圆角比较协调（原来 16 在半高的条子上会变成胶囊）。
        HRGN region = ::CreateRoundRectRgn(0, 0, width, height, 12, 12);
        if (region != nullptr)
        {
            if (::SetWindowRgn(m_tipWindow, region, TRUE) == 0)
            {
                ::DeleteObject(region);   // 设置失败要自己收；成功的话归窗口管
            }
        }

        ::InvalidateRect(m_tipWindow, nullptr, FALSE);
        ::UpdateWindow(m_tipWindow);   // 拖动中消息循环在 OLE 手里，立刻画出来最稳
    }

    void HideDropTip()
    {
        if (m_tipWindow != nullptr)
        {
            ::DestroyWindow(m_tipWindow);
            m_tipWindow = nullptr;
        }

        m_tipText.clear();
        m_tipX = 0;
        m_tipY = 0;
    }

    // 目录在系统里的显示名（"桌面"、"文档"、"下载"、"本地磁盘 (C:)"……）
    static std::wstring ShellDisplayName(const std::wstring& path)
    {
        SHFILEINFOW info = {};
        if (::SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_DISPLAYNAME) != 0 &&
            info.szDisplayName[0] != L'\0')
        {
            return info.szDisplayName;
        }

        const size_t slash = path.find_last_of(L"\\/");
        return (slash != std::wstring::npos) ? path.substr(slash + 1) : path;
    }

    // 把拖动过程中的"当前落点"告诉宿主（文件列表用它高亮鼠标正压着的那个文件夹）
    //   pt              ：屏幕坐标（DragLeave 时无效，宿主只看 acceptable）
    //   acceptable      ：这个位置能不能放
    void NotifyFeedback(POINTL pt, bool acceptable = true)
    {
        if (!m_callbacks.DragOverFeedback)
        {
            return;
        }

        POINT clientPoint = { pt.x, pt.y };

        if (acceptable)
        {
            ::ScreenToClient(m_hwnd, &clientPoint);
            acceptable = !ResolveDirectory(pt).empty();
        }

        m_callbacks.DragOverFeedback(clientPoint, acceptable);
    }

    // 从数据对象里取出文件路径列表
    std::vector<std::wstring> ExtractPaths(IDataObject* data)
    {
        std::vector<std::wstring> paths;
        if (data == nullptr)
        {
            return paths;
        }

        FORMATETC format = {};
        format.cfFormat = static_cast<CLIPFORMAT>(CF_HDROP);
        format.dwAspect = DVASPECT_CONTENT;
        format.lindex   = -1;
        format.tymed    = TYMED_HGLOBAL;

        STGMEDIUM medium = {};
        if (FAILED(data->GetData(&format, &medium)))
        {
            return paths;
        }

        HDROP drop = static_cast<HDROP>(::GlobalLock(medium.hGlobal));
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

                std::wstring path(length + 1, L'\0');
                ::DragQueryFileW(drop, i, path.data(), length + 1);
                path.resize(length);
                paths.push_back(path);
            }

            ::GlobalUnlock(medium.hGlobal);
        }

        ::ReleaseStgMedium(&medium);
        return paths;
    }

    // 取拖动源声明的首选效果
    DWORD ExtractPreferredEffect(IDataObject* data)
    {
        UINT effectFormat = PreferredDropEffectFormat();
        if (data == nullptr || effectFormat == 0)
        {
            return 0;
        }

        FORMATETC format = {};
        format.cfFormat = static_cast<CLIPFORMAT>(effectFormat);
        format.dwAspect = DVASPECT_CONTENT;
        format.lindex   = -1;
        format.tymed    = TYMED_HGLOBAL;

        STGMEDIUM medium = {};
        if (FAILED(data->GetData(&format, &medium)))
        {
            return 0;
        }

        DWORD result = 0;
        void* raw = ::GlobalLock(medium.hGlobal);
        if (raw != nullptr)
        {
            result = *static_cast<DWORD*>(raw);
            ::GlobalUnlock(medium.hGlobal);
        }

        ::ReleaseStgMedium(&medium);
        return result;
    }

    // 把 POINTL（屏幕坐标）换成客户区 POINT，再问宿主"这里对应哪个目录"
    //
    // 注意：IDropTarget 回调里给的 pt 是**屏幕坐标**，不是客户区坐标。
    // 直接拿它去 HitTest 会落在错误的位置（左栏就是这么没接住拖放的）。
    std::wstring ResolveDirectory(POINTL pt) const
    {
        if (!m_callbacks.ResolveTargetDirectory)
        {
            return std::wstring();
        }

        POINT clientPoint = { pt.x, pt.y };
        ::ScreenToClient(m_hwnd, &clientPoint);

        return m_callbacks.ResolveTargetDirectory(clientPoint);
    }

    // 落点是不是「源自己」或「源里面的子目录」
    //
    // 任意一项命中就算命中：拖动光标和提示条只能表达一个状态，
    // 这种情况下整体按"不可放置"处理（复制/移动都会失败并弹系统错误框）。
    static bool IsDropIntoSelf(const std::wstring&              targetDirectory,
                               const std::vector<std::wstring>& sources)
    {
        for (const std::wstring& source : sources)
        {
            if (IsSelfOrDescendantPath(targetDirectory, source))
            {
                return true;
            }
        }

        return false;
    }

    void UpdateEffect(DWORD keyState, POINTL pt, DWORD* effect)
    {
        std::wstring targetDirectory = ResolveDirectory(pt);

        if (m_sourcePaths.empty() || targetDirectory.empty())
        {
            if (effect != nullptr)
            {
                *effect = DROPEFFECT_NONE;
            }
            return;
        }

        if (effect != nullptr)
        {
            // 我们自己的放置目标知道支持哪些效果，所以这里直接用算出来的结果，
            // 不再跟系统给的 allowed 求交（否则按 Alt 拖到自己左栏会被挡掉）
            DWORD computed = ComputeDropEffect(keyState, m_sourcePreferredEffect,
                                              IsSameDrive(m_sourcePaths.front(), targetDirectory));

            // 落点是「源自己」或「源里面的子目录」时，复制和移动在系统层面都做不了
            // （IFileOperation 会报错并弹它自己的错误框），所以拖动阶段就判死：
            // 光标变禁止，提示条显示「⊘ 不能放到 此处」。
            // 建快捷方式（Alt）放行 —— 在自己里面建一个指向自己的 .lnk 是合法的。
            if (computed != DROPEFFECT_LINK && IsDropIntoSelf(targetDirectory, m_sourcePaths))
            {
                computed = DROPEFFECT_NONE;
            }

            *effect = computed;
        }
    }

private:
    LONG                      m_refCount = 1;
    HWND                      m_hwnd;
    DropTargetCallbacks       m_callbacks;
    std::vector<std::wstring> m_sourcePaths;            // 正在拖过来的东西（DragEnter 时记下）
    DWORD                     m_sourcePreferredEffect = 0;

    HWND         m_tipWindow = nullptr;   // 自己画的那条提示（跟着光标右下角走）
    std::wstring m_tipText;               // 当前提示文字
    int          m_tipX = 0;              // 当前窗口位置（用来判断要不要挪）
    int          m_tipY = 0;
};

// ============================================================================
// 注册 / 注销
// ============================================================================

bool RegisterWindowDropTarget(HWND hwnd, const DropTargetCallbacks& callbacks)
{
    if (hwnd == nullptr)
    {
        return false;
    }

    // 同一个窗口重复注册：先注销旧的
    UnregisterWindowDropTarget(hwnd);

    WindowDropTarget* target = new WindowDropTarget(hwnd, callbacks);

    HRESULT hr = ::RegisterDragDrop(hwnd, target);
    if (FAILED(hr))
    {
        Logger::Warn(L"注册放置目标失败：" + Logger::FormatHresult(static_cast<long>(hr)));
        target->Release();
        return false;
    }

    g_dropTargets[hwnd] = target;   // 注册成功后系统已经持有一份引用
    return true;
}

void UnregisterWindowDropTarget(HWND hwnd)
{
    auto found = g_dropTargets.find(hwnd);
    if (found == g_dropTargets.end())
    {
        return;
    }

    ::RevokeDragDrop(hwnd);
    found->second->Release();
    g_dropTargets.erase(found);
}

} // namespace shell
