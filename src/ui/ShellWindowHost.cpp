// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/ui/ShellWindowHost.cpp
//
// 实现见头文件。核心是：一个最小 IDispatch 自动化对象 + IShellWindows 注册。
// ============================================================================

#include "ShellWindowHost.h"

#include <exdisp.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <oleauto.h>

#include <string>

#include "core/Logger.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "oleaut32.lib")

namespace ui
{

namespace
{

// 自动化对象的 DISPID。
// 这些值必须和 shdocvw 类型库里 IWebBrowser2 的定义一致：
// Shell.Application / IShellWindows 是按标准 DISPID 调用的，自己编的 ID 它们不认。
enum DispId : DISPID
{
    kDispName               = 0,
    kDispGoBack             = 100,
    kDispGoForward          = 101,
    kDispGoHome             = 102,
    kDispGoSearch           = 103,
    kDispNavigate           = 104,
    kDispRefresh2           = 105,
    kDispStop               = 106,
    kDispApplication        = 200,
    kDispParent             = 201,
    kDispContainer          = 202,
    kDispDocument           = 203,
    kDispTopLevelContainer  = 204,
    kDispType               = 205,
    kDispLeft               = 206,
    kDispTop                = 207,
    kDispWidth              = 208,
    kDispHeight             = 209,
    kDispLocationName       = 210,
    kDispLocationURL        = 211,
    kDispBusy               = 212,
    kDispQuit               = 300,
    kDispClientToWindow     = 301,
    kDispPutProperty        = 302,
    kDispGetProperty        = 303,
    kDispFullName           = 400,
    kDispPath               = 401,
    kDispVisible            = 402,
    kDispStatusBar          = 403,
    kDispStatusText         = 404,
    kDispToolBar            = 405,
    kDispMenuBar            = 406,
    kDispFullScreen         = 407,
    kDispNavigate2          = 500,
    kDispQueryStatusWB      = 501,
    kDispExecWB             = 502,
    kDispShowBrowserBar     = 503,
    kDispHWND               = -515,
    kDispReadyState         = -525,
    kDispRefresh            = -550,
    kDispOffline            = 550,
    kDispSilent             = 551,
    kDispRegisterAsBrowser  = 552,
    kDispRegisterAsDropTarget = 553,
    kDispTheaterMode        = 554,
    kDispAddressBar         = 555,
    kDispResizable          = 556,
};

struct DispName
{
    const wchar_t* name;
    DISPID         id;
};

const DispName kDispNames[] = {
    { L"Application",           kDispApplication },
    { L"Parent",                kDispParent },
    { L"Container",             kDispContainer },
    { L"Document",              kDispDocument },
    { L"TopLevelContainer",     kDispTopLevelContainer },
    { L"Type",                  kDispType },
    { L"Left",                  kDispLeft },
    { L"Top",                   kDispTop },
    { L"Width",                 kDispWidth },
    { L"Height",                kDispHeight },
    { L"LocationName",          kDispLocationName },
    { L"LocationURL",           kDispLocationURL },
    { L"Busy",                  kDispBusy },
    { L"Name",                  kDispName },
    { L"HWND",                  kDispHWND },
    { L"FullName",              kDispFullName },
    { L"Path",                  kDispPath },
    { L"Visible",               kDispVisible },
    { L"StatusBar",             kDispStatusBar },
    { L"StatusText",            kDispStatusText },
    { L"ToolBar",               kDispToolBar },
    { L"MenuBar",               kDispMenuBar },
    { L"FullScreen",            kDispFullScreen },
    { L"ReadyState",            kDispReadyState },
    { L"Offline",               kDispOffline },
    { L"Silent",                kDispSilent },
    { L"RegisterAsBrowser",     kDispRegisterAsBrowser },
    { L"RegisterAsDropTarget",  kDispRegisterAsDropTarget },
    { L"TheaterMode",           kDispTheaterMode },
    { L"AddressBar",            kDispAddressBar },
    { L"Resizable",             kDispResizable },
    { L"Navigate",              kDispNavigate },
    { L"Navigate2",             kDispNavigate2 },
    { L"GoBack",                kDispGoBack },
    { L"GoForward",             kDispGoForward },
    { L"GoHome",                kDispGoHome },
    { L"GoSearch",              kDispGoSearch },
    { L"Refresh",               kDispRefresh },
    { L"Refresh2",              kDispRefresh2 },
    { L"Stop",                  kDispStop },
    { L"Quit",                  kDispQuit },
    { L"ClientToWindow",        kDispClientToWindow },
    { L"PutProperty",           kDispPutProperty },
    { L"GetProperty",           kDispGetProperty },
    { L"QueryStatusWB",         kDispQueryStatusWB },
    { L"ExecWB",                kDispExecWB },
    { L"ShowBrowserBar",        kDispShowBrowserBar },
};

std::wstring FileUrlOf(const std::wstring& folder)
{
    if (folder.empty())
    {
        return std::wstring();
    }

    wchar_t url[2048] = {};
    DWORD length = ARRAYSIZE(url);
    if (SUCCEEDED(::UrlCreateFromPathW(folder.c_str(), url, &length, 0)))
    {
        return url;
    }
    return folder;
}

std::wstring PathOfUrlOrPath(const std::wstring& text)
{
    if (text.empty())
    {
        return std::wstring();
    }

    if (_wcsnicmp(text.c_str(), L"file:", 5) == 0)
    {
        wchar_t path[MAX_PATH * 2] = {};
        DWORD length = ARRAYSIZE(path);
        if (SUCCEEDED(::PathCreateFromUrlW(text.c_str(), path, &length, 0)))
        {
            return path;
        }
        return std::wstring();
    }

    // 只接受本地绝对路径，其它 URL（http 等）不接管
    if ((text.size() >= 3 && text[1] == L':' && (text[2] == L'\\' || text[2] == L'/')) ||
        (text.size() >= 2 && text[0] == L'\\' && text[1] == L'\\'))
    {
        return text;
    }
    return std::wstring();
}

HRESULT ReturnBstr(const std::wstring& text, VARIANT* result)
{
    if (result == nullptr)
    {
        return S_OK;
    }
    ::VariantInit(result);
    result->vt      = VT_BSTR;
    result->bstrVal = ::SysAllocString(text.c_str());
    return result->bstrVal != nullptr ? S_OK : E_OUTOFMEMORY;
}

HRESULT ReturnBool(bool value, VARIANT* result)
{
    if (result == nullptr)
    {
        return S_OK;
    }
    ::VariantInit(result);
    result->vt      = VT_BOOL;
    result->boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

HRESULT ReturnLong(long value, VARIANT* result)
{
    if (result == nullptr)
    {
        return S_OK;
    }
    ::VariantInit(result);
    result->vt   = VT_I4;
    result->lVal = value;
    return S_OK;
}

} // namespace

// ----------------------------------------------------------------------------
// 自动化对象：只实现"读目录 / 关窗 / 刷新 / 导航"这一小撮，其余 E_NOTIMPL。
// ----------------------------------------------------------------------------
class ShellWindowHost::Automation final : public IWebBrowser2
{
public:
    explicit Automation(ShellWindowHost* host) : m_host(host) {}

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
    {
        if (ppvObject == nullptr)
        {
            return E_POINTER;
        }

        if (::IsEqualIID(riid, IID_IUnknown) || ::IsEqualIID(riid, IID_IDispatch) ||
            ::IsEqualIID(riid, IID_IWebBrowser) || ::IsEqualIID(riid, IID_IWebBrowserApp) ||
            ::IsEqualIID(riid, IID_IWebBrowser2))
        {
            *ppvObject = static_cast<IWebBrowser2*>(this);
            AddRef();
            return S_OK;
        }

        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(::InterlockedIncrement(&m_ref));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG ref = ::InterlockedDecrement(&m_ref);
        if (ref == 0)
        {
            delete this;
        }
        return static_cast<ULONG>(ref);
    }

    // ---- IDispatch ----
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* pctinfo) override
    {
        if (pctinfo == nullptr)
        {
            return E_POINTER;
        }
        *pctinfo = 1;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT index, LCID, ITypeInfo** pptinfo) override
    {
        if (pptinfo == nullptr)
        {
            return E_POINTER;
        }
        *pptinfo = nullptr;
        if (index != 0)
        {
            return DISP_E_BADINDEX;
        }

        // 返回 shdocvw 类型库里 IWebBrowser2 的 ITypeInfo：
        // 跨进程调用要靠它按双接口封送，FindWindowSW/自动化客户端都认这个。
        ITypeLib* library = nullptr;
        if (FAILED(::LoadRegTypeLib(LIBID_SHDocVw, 1, 0, 0, &library)) || library == nullptr)
        {
            return E_FAIL;
        }

        HRESULT hr = library->GetTypeInfoOfGuid(IID_IWebBrowser2, pptinfo);
        library->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR* rgszNames, UINT cNames,
                                            LCID, DISPID* rgDispId) override
    {
        if (rgszNames == nullptr || rgDispId == nullptr)
        {
            return E_POINTER;
        }

        for (UINT i = 0; i < cNames; ++i)
        {
            DISPID found = DISPID_UNKNOWN;

            for (const DispName& entry : kDispNames)
            {
                if (::_wcsicmp(rgszNames[i], entry.name) == 0)
                {
                    found = entry.id;
                    break;
                }
            }

            if (found == DISPID_UNKNOWN)
            {
                return DISP_E_UNKNOWNNAME;
            }
            rgDispId[i] = found;
        }

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Invoke(DISPID dispIdMember, REFIID, LCID, WORD wFlags,
                                     DISPPARAMS* pDispParams, VARIANT* pVarResult,
                                     EXCEPINFO*, UINT*) override
    {
        (void)wFlags;

        if (m_host == nullptr)
        {
            return E_FAIL;
        }

        const bool isGet = (wFlags & DISPATCH_PROPERTYGET) != 0;
        const bool isPut = (wFlags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)) != 0;

        switch (dispIdMember)
        {
        case kDispApplication:
        case kDispParent:
        case kDispContainer:
            return ReturnBstr(L"", pVarResult);   // 不提供父对象

        case kDispDocument:
            return E_NOTIMPL;                     // 不做窗口内容自动化

        case kDispTopLevelContainer:
            return ReturnBool(true, pVarResult);

        case kDispType:
            return ReturnBstr(L"Yexplorer", pVarResult);

        case kDispLeft:
        case kDispTop:
        case kDispWidth:
        case kDispHeight:
        case kDispStatusBar:
        case kDispToolBar:
        case kDispMenuBar:
        case kDispFullScreen:
        case kDispOffline:
        case kDispSilent:
        case kDispRegisterAsBrowser:
        case kDispRegisterAsDropTarget:
        case kDispTheaterMode:
        case kDispAddressBar:
        case kDispResizable:
        case kDispStatusText:
            if (isGet)
            {
                return ReturnLong(0, pVarResult);
            }
            return S_OK;                          // 写入忽略

        case kDispLocationName:
            return ReturnBstr(m_host->Folder(), pVarResult);

        case kDispLocationURL:
            return ReturnBstr(FileUrlOf(m_host->Folder()), pVarResult);

        case kDispBusy:
            return ReturnBool(false, pVarResult);

        case kDispReadyState:
            return ReturnLong(4 /* READYSTATE_COMPLETE */, pVarResult);

        case kDispName:
            return ReturnBstr(L"Yexplorer", pVarResult);

        case kDispFullName:
            return ReturnBstr(ShellWindowHost::OwnExePath(), pVarResult);

        case kDispPath:
        {
            std::wstring exe = ShellWindowHost::OwnExePath();
            const size_t slash = exe.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
            {
                exe.erase(slash);
            }
            return ReturnBstr(exe, pVarResult);
        }

        case kDispHWND:
            return ReturnLong(static_cast<long>(reinterpret_cast<LONG_PTR>(m_host->Window())),
                              pVarResult);

        case kDispVisible:
        {
            if (isPut && pDispParams != nullptr && pDispParams->cArgs >= 1)
            {
                const VARIANT* value = &pDispParams->rgvarg[0];
                const bool visible = (value->vt == VT_BOOL)
                                         ? (value->boolVal != VARIANT_FALSE)
                                         : (value->lVal != 0);
                ::ShowWindow(m_host->Window(), visible ? SW_SHOW : SW_HIDE);
                return S_OK;
            }
            return ReturnBool(::IsWindowVisible(m_host->Window()) != FALSE, pVarResult);
        }

        case kDispQuit:
            ::PostMessageW(m_host->Window(), WM_CLOSE, 0, 0);
            return S_OK;

        case kDispRefresh:
            ::PostMessageW(m_host->Window(), kShellWindowRefreshMessage, 0, 0);
            return S_OK;

        case kDispStop:
        case kDispGoBack:
        case kDispGoForward:
        case kDispGoHome:
        case kDispGoSearch:
        case kDispClientToWindow:
        case kDispPutProperty:
        case kDispGetProperty:
        case kDispQueryStatusWB:
        case kDispExecWB:
        case kDispShowBrowserBar:
            return S_OK;

        case kDispNavigate:
        case kDispNavigate2:
        {
            // 第一个参数是 URL；DISPID 参数数组是倒序的
            const VARIANT* url = nullptr;
            if (pDispParams != nullptr && pDispParams->cArgs >= 1)
            {
                url = &pDispParams->rgvarg[pDispParams->cArgs - 1];
            }
            if (url == nullptr)
            {
                return E_INVALIDARG;
            }

            std::wstring text;
            if (url->vt == VT_BSTR && url->bstrVal != nullptr)
            {
                text = url->bstrVal;
            }
            else if (url->vt == VT_DISPATCH || url->vt == VT_UNKNOWN)
            {
                return E_NOTIMPL;   // 不处理 pidl 形式的导航
            }
            else
            {
                return E_INVALIDARG;
            }

            const std::wstring path = PathOfUrlOrPath(text);
            if (path.empty())
            {
                return E_INVALIDARG;
            }

            auto* payload = new std::wstring(path);
            if (::PostMessageW(m_host->Window(), kShellWindowNavigateMessage, 0,
                               reinterpret_cast<LPARAM>(payload)) == FALSE)
            {
                delete payload;
                return E_FAIL;
            }
            return S_OK;
        }

        default:
            return DISP_E_MEMBERNOTFOUND;
        }
    }

#pragma warning(push)
#pragma warning(disable: 4100)   // 生成的 IWebBrowser2 空桩：参数用不到
    /* [helpcontext][helpstring][id] */ HRESULT STDMETHODCALLTYPE GoBack( void) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][id] */ HRESULT STDMETHODCALLTYPE GoForward( void) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][id] */ HRESULT STDMETHODCALLTYPE GoHome( void) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][id] */ HRESULT STDMETHODCALLTYPE GoSearch( void) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][id] */ HRESULT STDMETHODCALLTYPE Navigate( /* [in] */ __RPC__in BSTR URL, /* [unique][optional][in] */ __RPC__in_opt VARIANT *Flags, /* [unique][optional][in] */ __RPC__in_opt VARIANT *TargetFrameName, /* [unique][optional][in] */ __RPC__in_opt VARIANT *PostData, /* [unique][optional][in] */ __RPC__in_opt VARIANT *Headers) override { { const std::wstring path = PathOfUrlOrPath(URL != nullptr ? std::wstring(URL) : std::wstring()); if (path.empty()) return E_INVALIDARG; auto* payload = new std::wstring(path); if (!::PostMessageW(m_host->Window(), kShellWindowNavigateMessage, 0, reinterpret_cast<LPARAM>(payload))) { delete payload; return E_FAIL; } return S_OK; } }
    /* [helpcontext][helpstring][id] */ HRESULT STDMETHODCALLTYPE Refresh( void) override { ::PostMessageW(m_host->Window(), kShellWindowRefreshMessage, 0, 0); return S_OK; }
    /* [helpcontext][helpstring][id] */ HRESULT STDMETHODCALLTYPE Refresh2( /* [unique][optional][in] */ __RPC__in_opt VARIANT *Level) override { return S_OK; }
    /* [helpcontext][helpstring][id] */ HRESULT STDMETHODCALLTYPE Stop( void) override { return S_OK; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_Application( /* [retval][out] */ __RPC__deref_out_opt IDispatch **ppDisp) override { *ppDisp = nullptr; return S_OK; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_Parent( /* [retval][out] */ __RPC__deref_out_opt IDispatch **ppDisp) override { *ppDisp = nullptr; return S_OK; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_Container( /* [retval][out] */ __RPC__deref_out_opt IDispatch **ppDisp) override { *ppDisp = nullptr; return S_OK; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_Document( /* [retval][out] */ __RPC__deref_out_opt IDispatch **ppDisp) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_TopLevelContainer( /* [retval][out] */ __RPC__out VARIANT_BOOL *pBool) override { *pBool = VARIANT_TRUE; return S_OK; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_Type( /* [retval][out] */ __RPC__deref_out_opt BSTR *Type) override { *Type = ::SysAllocString(L"Yexplorer"); return *Type != nullptr ? S_OK : E_OUTOFMEMORY; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_Left( /* [retval][out] */ __RPC__out long *pl) override { *pl = 0; return S_OK; }
    /* [propput][id] */ HRESULT STDMETHODCALLTYPE put_Left( /* [in] */ long Left) override { return S_OK; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_Top( /* [retval][out] */ __RPC__out long *pl) override { *pl = 0; return S_OK; }
    /* [propput][id] */ HRESULT STDMETHODCALLTYPE put_Top( /* [in] */ long Top) override { return S_OK; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_Width( /* [retval][out] */ __RPC__out long *pl) override { *pl = 0; return S_OK; }
    /* [propput][id] */ HRESULT STDMETHODCALLTYPE put_Width( /* [in] */ long Width) override { return S_OK; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_Height( /* [retval][out] */ __RPC__out long *pl) override { *pl = 0; return S_OK; }
    /* [propput][id] */ HRESULT STDMETHODCALLTYPE put_Height( /* [in] */ long Height) override { return S_OK; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_LocationName( /* [retval][out] */ __RPC__deref_out_opt BSTR *LocationName) override { *LocationName = ::SysAllocString(m_host->Folder().c_str()); return *LocationName != nullptr ? S_OK : E_OUTOFMEMORY; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_LocationURL( /* [retval][out] */ __RPC__deref_out_opt BSTR *LocationURL) override { *LocationURL = ::SysAllocString(FileUrlOf(m_host->Folder()).c_str()); return *LocationURL != nullptr ? S_OK : E_OUTOFMEMORY; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_Busy( /* [retval][out] */ __RPC__out VARIANT_BOOL *pBool) override { *pBool = VARIANT_FALSE; return S_OK; }
    /* [helpcontext][helpstring][id] */ HRESULT STDMETHODCALLTYPE Quit( void) override { ::PostMessageW(m_host->Window(), WM_CLOSE, 0, 0); return S_OK; }
    /* [helpcontext][helpstring][id] */ HRESULT STDMETHODCALLTYPE ClientToWindow( /* [out][in] */ __RPC__inout int *pcx, /* [out][in] */ __RPC__inout int *pcy) override { return S_OK; }
    /* [helpcontext][helpstring][id] */ HRESULT STDMETHODCALLTYPE PutProperty( /* [in] */ __RPC__in BSTR Property, /* [in] */ VARIANT vtValue) override { return S_OK; }
    /* [helpcontext][helpstring][id] */ HRESULT STDMETHODCALLTYPE GetProperty( /* [in] */ __RPC__in BSTR Property, /* [retval][out] */ __RPC__out VARIANT *pvtValue) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_Name( /* [retval][out] */ __RPC__deref_out_opt BSTR *Name) override { *Name = ::SysAllocString(L"Yexplorer"); return *Name != nullptr ? S_OK : E_OUTOFMEMORY; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_HWND( /* [retval][out] */ __RPC__out SHANDLE_PTR *pHWND) override { *pHWND = reinterpret_cast<SHANDLE_PTR>(m_host->Window()); return S_OK; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_FullName( /* [retval][out] */ __RPC__deref_out_opt BSTR *FullName) override { *FullName = ::SysAllocString(ShellWindowHost::OwnExePath().c_str()); return *FullName != nullptr ? S_OK : E_OUTOFMEMORY; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_Path( /* [retval][out] */ __RPC__deref_out_opt BSTR *Path) override { { std::wstring exe = ShellWindowHost::OwnExePath(); const size_t slash = exe.find_last_of(L"\\/"); if (slash != std::wstring::npos) exe.erase(slash); *Path = ::SysAllocString(exe.c_str()); return *Path != nullptr ? S_OK : E_OUTOFMEMORY; } }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_Visible( /* [retval][out] */ __RPC__out VARIANT_BOOL *pBool) override { *pBool = ::IsWindowVisible(m_host->Window()) ? VARIANT_TRUE : VARIANT_FALSE; return S_OK; }
    /* [helpcontext][helpstring][propput][id] */ HRESULT STDMETHODCALLTYPE put_Visible( /* [in] */ VARIANT_BOOL Value) override { ::ShowWindow(m_host->Window(), Value != VARIANT_FALSE ? SW_SHOW : SW_HIDE); return S_OK; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_StatusBar( /* [retval][out] */ __RPC__out VARIANT_BOOL *pBool) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propput][id] */ HRESULT STDMETHODCALLTYPE put_StatusBar( /* [in] */ VARIANT_BOOL Value) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_StatusText( /* [retval][out] */ __RPC__deref_out_opt BSTR *StatusText) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propput][id] */ HRESULT STDMETHODCALLTYPE put_StatusText( /* [in] */ __RPC__in BSTR StatusText) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_ToolBar( /* [retval][out] */ __RPC__out int *Value) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propput][id] */ HRESULT STDMETHODCALLTYPE put_ToolBar( /* [in] */ int Value) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_MenuBar( /* [retval][out] */ __RPC__out VARIANT_BOOL *Value) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propput][id] */ HRESULT STDMETHODCALLTYPE put_MenuBar( /* [in] */ VARIANT_BOOL Value) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_FullScreen( /* [retval][out] */ __RPC__out VARIANT_BOOL *pbFullScreen) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propput][id] */ HRESULT STDMETHODCALLTYPE put_FullScreen( /* [in] */ VARIANT_BOOL bFullScreen) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][id] */ HRESULT STDMETHODCALLTYPE Navigate2( /* [in] */ __RPC__in VARIANT *URL, /* [unique][optional][in] */ __RPC__in_opt VARIANT *Flags, /* [unique][optional][in] */ __RPC__in_opt VARIANT *TargetFrameName, /* [unique][optional][in] */ __RPC__in_opt VARIANT *PostData, /* [unique][optional][in] */ __RPC__in_opt VARIANT *Headers) override { { if (URL == nullptr || URL->vt != VT_BSTR || URL->bstrVal == nullptr) return E_INVALIDARG; const std::wstring path = PathOfUrlOrPath(URL->bstrVal); if (path.empty()) return E_INVALIDARG; auto* payload = new std::wstring(path); if (!::PostMessageW(m_host->Window(), kShellWindowNavigateMessage, 0, reinterpret_cast<LPARAM>(payload))) { delete payload; return E_FAIL; } return S_OK; } }
    /* [helpcontext][helpstring][id] */ HRESULT STDMETHODCALLTYPE QueryStatusWB( /* [in] */ OLECMDID cmdID, /* [retval][out] */ __RPC__out OLECMDF *pcmdf) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][id] */ HRESULT STDMETHODCALLTYPE ExecWB( /* [in] */ OLECMDID cmdID, /* [in] */ OLECMDEXECOPT cmdexecopt, /* [unique][optional][in] */ __RPC__in_opt VARIANT *pvaIn, /* [unique][optional][out][in] */ __RPC__inout_opt VARIANT *pvaOut) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][id] */ HRESULT STDMETHODCALLTYPE ShowBrowserBar( /* [in] */ __RPC__in VARIANT *pvaClsid, /* [unique][optional][in] */ __RPC__in_opt VARIANT *pvarShow, /* [unique][optional][in] */ __RPC__in_opt VARIANT *pvarSize) override { return E_NOTIMPL; }
    /* [bindable][propget][id] */ HRESULT STDMETHODCALLTYPE get_ReadyState( /* [out][retval] */ __RPC__out READYSTATE *plReadyState) override { *plReadyState = READYSTATE_COMPLETE; return S_OK; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_Offline( /* [retval][out] */ __RPC__out VARIANT_BOOL *pbOffline) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propput][id] */ HRESULT STDMETHODCALLTYPE put_Offline( /* [in] */ VARIANT_BOOL bOffline) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_Silent( /* [retval][out] */ __RPC__out VARIANT_BOOL *pbSilent) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propput][id] */ HRESULT STDMETHODCALLTYPE put_Silent( /* [in] */ VARIANT_BOOL bSilent) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_RegisterAsBrowser( /* [retval][out] */ __RPC__out VARIANT_BOOL *pbRegister) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propput][id] */ HRESULT STDMETHODCALLTYPE put_RegisterAsBrowser( /* [in] */ VARIANT_BOOL bRegister) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_RegisterAsDropTarget( /* [retval][out] */ __RPC__out VARIANT_BOOL *pbRegister) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propput][id] */ HRESULT STDMETHODCALLTYPE put_RegisterAsDropTarget( /* [in] */ VARIANT_BOOL bRegister) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_TheaterMode( /* [retval][out] */ __RPC__out VARIANT_BOOL *pbRegister) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propput][id] */ HRESULT STDMETHODCALLTYPE put_TheaterMode( /* [in] */ VARIANT_BOOL bRegister) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_AddressBar( /* [retval][out] */ __RPC__out VARIANT_BOOL *Value) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propput][id] */ HRESULT STDMETHODCALLTYPE put_AddressBar( /* [in] */ VARIANT_BOOL Value) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propget][id] */ HRESULT STDMETHODCALLTYPE get_Resizable( /* [retval][out] */ __RPC__out VARIANT_BOOL *Value) override { return E_NOTIMPL; }
    /* [helpcontext][helpstring][propput][id] */ HRESULT STDMETHODCALLTYPE put_Resizable( /* [in] */ VARIANT_BOOL Value) override { return E_NOTIMPL; }

#pragma warning(pop)

private:
    ~Automation() = default;

    LONG             m_ref  = 1;
    ShellWindowHost* m_host = nullptr;
};

// ----------------------------------------------------------------------------
// ShellWindowHost
// ----------------------------------------------------------------------------
ShellWindowHost::ShellWindowHost() = default;

ShellWindowHost::~ShellWindowHost()
{
    Revoke();
}

bool ShellWindowHost::Register(HWND window, const std::wstring& folder)
{
    if (window == nullptr)
    {
        return false;
    }
    if (m_cookie != 0)
    {
        UpdateFolder(folder);
        return true;
    }

    IShellWindows* shellWindows = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
                                  IID_PPV_ARGS(&shellWindows))))
    {
        Logger::Info(L"Shell 窗口注册：拿不到 IShellWindows，跳过");
        return false;
    }
    m_shellWindows.attach(shellWindows);

    m_window = window;
    m_folder = folder;

    // 自动化对象自己管理引用计数；com_ptr 接管初始的 1 个引用
    m_automation.attach(new Automation(this));

    long cookie = 0;
    const HRESULT hr = m_shellWindows->Register(
        m_automation.get(), static_cast<long>(reinterpret_cast<LONG_PTR>(window)),
        SWC_EXPLORER, &cookie);
    if (FAILED(hr) || cookie == 0)
    {
        Logger::Info(L"Shell 窗口注册失败（HRESULT=" + Logger::FormatHresult(hr) + L"）");
        m_automation.reset();
        m_shellWindows.reset();
        m_window = nullptr;
        m_folder.clear();
        return false;
    }

    m_cookie = cookie;
    Logger::Info(L"Shell 窗口已注册，目录：" + folder);
    return true;
}

void ShellWindowHost::UpdateFolder(const std::wstring& folder)
{
    m_folder = folder;
}

void ShellWindowHost::Revoke()
{
    if (m_shellWindows && m_cookie != 0)
    {
        m_shellWindows->Revoke(m_cookie);
        Logger::Info(L"Shell 窗口已注销");
    }

    m_cookie = 0;
    m_automation.reset();
    m_shellWindows.reset();
    m_window = nullptr;
    m_folder.clear();
}

std::wstring ShellWindowHost::OwnExePath()
{
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    return std::wstring(path, length);
}

} // namespace ui
