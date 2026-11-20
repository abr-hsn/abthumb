// IThumbnailProvider: forwards the request to the agent; logs the QI / Initialize calls (experiment E1).
#include "shellext.h"
#include "abt_wic.h"
#include "abt_proto.h"
#include <thumbcache.h>
#include <propsys.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <stdio.h>

namespace {

void log_iid(const char* what, REFIID riid) {
    if (!abt::log_enabled()) return;
    wchar_t s[64];
    StringFromGUID2(riid, s, 64);
    abt::logf("%s %ls", what, s);
}

class ThumbnailProvider final : public IThumbnailProvider,
                                public IInitializeWithItem,
                                public IInitializeWithStream,
                                public IInitializeWithFile {
    LONG m_ref = 1;
    wchar_t* m_path = nullptr;  // CoTaskMem
    IStream* m_stream = nullptr;

public:
    ThumbnailProvider() { dll_addref(); }
    ~ThumbnailProvider() {
        if (m_stream) m_stream->Release();
        CoTaskMemFree(m_path);
        dll_release();
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        log_iid("thumb QI", riid);
        *ppv = nullptr;
        const ExposeFlags& ex = expose_flags();
        if (riid == IID_IUnknown || riid == IID_IThumbnailProvider)
            *ppv = static_cast<IThumbnailProvider*>(this);
        else if (riid == IID_IInitializeWithItem && ex.item)
            *ppv = static_cast<IInitializeWithItem*>(this);
        else if (riid == IID_IInitializeWithStream && ex.stream)
            *ppv = static_cast<IInitializeWithStream*>(this);
        else if (riid == IID_IInitializeWithFile && ex.file)
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
        if (m_path || m_stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &m_path);
        abt::logf("thumb Initialize(item) mode=%lu hr=0x%08lx path=%ls", mode, hr, m_path ? m_path : L"(null)");
        return hr;
    }

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* stream, DWORD mode) override {
        if (m_path || m_stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        m_stream = stream;
        m_stream->AddRef();
        if (abt::log_enabled()) {
            STATSTG st = {};
            HRESULT hr = stream->Stat(&st, STATFLAG_DEFAULT);
            abt::logf("thumb Initialize(stream) mode=%lu stat=0x%08lx name=%ls size=%llu", mode, hr,
                      st.pwcsName ? st.pwcsName : L"(null)", (unsigned long long)st.cbSize.QuadPart);
            CoTaskMemFree(st.pwcsName);
        }
        return S_OK;
    }

    // IInitializeWithFile
    IFACEMETHODIMP Initialize(LPCWSTR path, DWORD mode) override {
        if (m_path || m_stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        HRESULT hr = SHStrDupW(path, &m_path);
        abt::logf("thumb Initialize(file) mode=%lu path=%ls", mode, path);
        return hr;
    }

    // IThumbnailProvider: one round trip to the agent, then copy into a DIB section.
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* out, WTS_ALPHATYPE* alpha) override {
        uint64_t t0 = abt::qpc();
        *out = nullptr;
        *alpha = WTSAT_UNKNOWN;
        if (!m_path || !cx || cx > 4096) return E_INVALIDARG;
        abt_resp resp;
        uint8_t* px = nullptr;
        HRESULT hr = request_image(ABT_MSG_THUMB, m_path, cx, cx, 0, &resp, &px);
        if (SUCCEEDED(hr) && px) {
            hr = make_bitmap(px, (int)resp.width, (int)resp.height, out, alpha);
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
    abt::log_init(L"shellext");
    ThumbnailProvider* p = new (std::nothrow) ThumbnailProvider();
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release();
    return hr;
}
