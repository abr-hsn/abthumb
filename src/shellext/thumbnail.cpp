// IThumbnailProvider: forwards the request to the agent. IInitializeWithItem / IInitializeWithFile only:
// the shell prefers IInitializeWithStream when offered, and a stream carries no directory (D-007).
#include "shellext.h"
#include "abt_wic.h"
#include "abt_proto.h"
#include <thumbcache.h>
#include <propsys.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <stdio.h>

namespace {

class ThumbnailProvider final : public IThumbnailProvider,
                                public IInitializeWithItem,
                                public IInitializeWithFile {
    volatile LONG m_ref = 1;
    wchar_t* m_path = nullptr;  // CoTaskMem

public:
    ThumbnailProvider() { dll_addref(); }
    ~ThumbnailProvider() {
        CoTaskMemFree(m_path);
        dll_release();
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IThumbnailProvider)
            *ppv = static_cast<IThumbnailProvider*>(this);
        else if (riid == IID_IInitializeWithItem)
            *ppv = static_cast<IInitializeWithItem*>(this);
        else if (riid == IID_IInitializeWithFile)
            *ppv = static_cast<IInitializeWithFile*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return (ULONG)r;
    }

    // IInitializeWithItem
    IFACEMETHODIMP Initialize(IShellItem* item, DWORD mode) override {
        if (!item) return E_INVALIDARG;
        if (m_path) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &m_path);
        abt::logf("thumb Initialize(item) mode=%lu hr=0x%08lx path=%ls", mode, hr, m_path ? m_path : L"(null)");
        return hr;
    }

    // IInitializeWithFile
    IFACEMETHODIMP Initialize(LPCWSTR path, DWORD mode) override {
        if (!path) return E_INVALIDARG;
        if (m_path) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        HRESULT hr = SHStrDupW(path, &m_path);
        abt::logf("thumb Initialize(file) mode=%lu path=%ls", mode, path);
        return hr;
    }

    // IThumbnailProvider: one round trip to the agent, then copy into a DIB section.
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* out, WTS_ALPHATYPE* alpha) override {
        if (!out || !alpha) return E_POINTER;
        uint64_t t0 = abt::qpc();
        *out = nullptr;
        *alpha = WTSAT_UNKNOWN;
        if (!m_path) return E_UNEXPECTED;
        if (!cx || cx > 4096) return E_INVALIDARG;
        abt_resp resp;
        uint8_t* px = nullptr;
        HRESULT hr = request_image(ABT_MSG_THUMB, m_path, cx, cx, 0, &resp, &px);
        if (SUCCEEDED(hr) && px) {
            int has_alpha = (resp.source & ABT_SRC_HAS_ALPHA) ? 1 : 0;
            hr = make_bitmap(px, (int)resp.width, (int)resp.height, out, alpha, has_alpha);
        } else if (SUCCEEDED(hr)) {
            hr = E_FAIL;
        }
        abt::mem_free(px);
        abt::logf("thumb cx=%u %ls -> 0x%08lx %ux%u src=0x%x %.2f ms (%s)", cx, PathFindFileNameW(m_path), hr,
                  resp.width, resp.height, resp.source, abt::qpc_ms(t0, abt::qpc()), resp.reason);
        return hr;
    }
};

}  // namespace

HRESULT create_thumbnail_provider(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    abt::log_init(L"shellext");
    ThumbnailProvider* p = new (std::nothrow) ThumbnailProvider();
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release();
    return hr;
}
