// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/ShellDragDrop.cpp
//
// 拖放实现（详见头文件说明）。
// ============================================================================

#include "ShellDragDrop.h"

#include "core/Logger.h"

#include <commoncontrols.h>
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

        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** enumFormat) override
    {
        if (enumFormat == nullptr)
        {
            return E_POINTER;
        }

        // 除了"取数据"，也要支持"写回首选效果"
        if (direction == DATADIR_SET)
        {
            FORMATETC setFormat = {};
            setFormat.cfFormat = static_cast<CLIPFORMAT>(PreferredDropEffectFormat());
            setFormat.dwAspect = DVASPECT_CONTENT;
            setFormat.lindex   = -1;
            setFormat.tymed    = TYMED_HGLOBAL;
            return ::SHCreateStdEnumFmtEtc(1, &setFormat, enumFormat);
        }

        if (direction != DATADIR_GET)
        {
            *enumFormat = nullptr;
            return E_NOTIMPL;
        }

        FORMATETC formats[2] = {};
        formats[0].cfFormat = static_cast<CLIPFORMAT>(CF_HDROP);
        formats[0].dwAspect = DVASPECT_CONTENT;
        formats[0].lindex   = -1;
        formats[0].tymed    = TYMED_HGLOBAL;

        if (m_preferredEffect == 0)
        {
            // 没有首选效果时只声明 CF_HDROP 一种格式
            return ::SHCreateStdEnumFmtEtc(1, formats, enumFormat);
        }

        formats[1].cfFormat = static_cast<CLIPFORMAT>(PreferredDropEffectFormat());
        formats[1].dwAspect = DVASPECT_CONTENT;
        formats[1].lindex   = -1;
        formats[1].tymed    = TYMED_HGLOBAL;

        return ::SHCreateStdEnumFmtEtc(2, formats, enumFormat);
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
    LONG                      m_refCount = 1;
    std::vector<std::wstring> m_paths;
    DWORD                     m_preferredEffect;
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
                  DWORD                           allowedEffects)
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
        dataObject.attach(new SimpleDataObject(paths, 0));
    }
    if (dataObject == nullptr)
    {
        return DROPEFFECT_NONE;
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
}

class WindowDropTarget final : public IDropTarget
{
public:
    WindowDropTarget(HWND hwnd, DropTargetCallbacks callbacks)
        : m_hwnd(hwnd)
        , m_callbacks(std::move(callbacks))
    {
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

        Logger::Info(L"拖入进入：源 " + std::to_wstring(m_sourcePaths.size()) +
                     L" 项，落点目录=" + ResolveDirectory(pt) +
                     L"，效果=" + std::to_wstring((effect != nullptr) ? *effect : 0));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD keyState, POINTL pt, DWORD* effect) override
    {
        UpdateEffect(keyState, pt, effect);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        m_sourcePaths.clear();
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
            return S_OK;
        }

        DWORD chosen = ComputeDropEffect(keyState, ExtractPreferredEffect(data),
                                        IsSameDrive(sources.front(), targetDirectory));

        bool createShortcut = (chosen == DROPEFFECT_LINK);

        if (effect != nullptr)
        {
            *effect = chosen;
        }

        if (m_callbacks.PerformDrop)
        {
            m_callbacks.PerformDrop(sources, targetDirectory, chosen, createShortcut);
        }

        m_sourcePaths.clear();
        return S_OK;
    }

private:
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
            *effect = ComputeDropEffect(keyState, m_sourcePreferredEffect,
                                       IsSameDrive(m_sourcePaths.front(), targetDirectory));
        }
    }

private:
    LONG                      m_refCount = 1;
    HWND                      m_hwnd;
    DropTargetCallbacks       m_callbacks;
    std::vector<std::wstring> m_sourcePaths;            // 正在拖过来的东西（DragEnter 时记下）
    DWORD                     m_sourcePreferredEffect = 0;
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
