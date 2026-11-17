// IThumbnailProvider. Step 2: placeholder output + experiment-E1 logging.
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
    const char* m_init = "none";

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
        m_init = "item";
        abt::logf("thumb Initialize(item) mode=%lu hr=0x%08lx path=%ls", mode, hr, m_path ? m_path : L"(null)");
        return hr;
    }

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* stream, DWORD mode) override {
        if (m_path || m_stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        m_stream = stream;
        m_stream->AddRef();
        m_init = "stream";
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
        m_init = "file";
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

void draw_placeholder(void* bits, int w, int h, const wchar_t* path, const wchar_t* line2) {
    const wchar_t* ext = abt::path_ext(path);
    COLORREF bg = RGB(90, 90, 90);
    if (!lstrcmpiW(ext, L".ai") || !lstrcmpiW(ext, L".ait")) bg = RGB(255, 154, 0);
    else if (!lstrcmpiW(ext, L".eps") || !lstrcmpiW(ext, L".epsf") || !lstrcmpiW(ext, L".epsi")) bg = RGB(200, 120, 40);
    else if (!lstrcmpiW(ext, L".psd") || !lstrcmpiW(ext, L".psb") || !lstrcmpiW(ext, L".psdt") || !lstrcmpiW(ext, L".pdd"))
        bg = RGB(49, 168, 255);
    else if (!lstrcmpiW(ext, L".pdf")) bg = RGB(220, 40, 40);

    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    // Draw into a temporary DIB we own, then copy (GDI needs the bitmap selected into a DC).
    void* tmp_bits = nullptr;
    HBITMAP tmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &tmp_bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, tmp);
    RECT rc = {0, 0, w, h};
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(dc, &rc, br);
    DeleteObject(br);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    int fh = h / 6 > 10 ? h / 6 : 10;
    HFONT f1 = CreateFontW(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, f1);
    RECT r1 = {0, h / 4, w, h / 2 + fh / 2};
    wchar_t label[16];
    lstrcpynW(label, *ext ? ext + 1 : L"?", 16);
    CharUpperW(label);
    DrawTextW(dc, label, -1, &r1, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    HFONT f2 = CreateFontW(-(fh / 2 > 8 ? fh / 2 : 8), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SelectObject(dc, f2);
    RECT r2 = {0, h / 2 + fh / 2, w, h};
    DrawTextW(dc, line2, -1, &r2, DT_CENTER | DT_SINGLELINE | DT_TOP);
    SelectObject(dc, of);
    DeleteObject(f1);
    DeleteObject(f2);
    GdiFlush();
    uint8_t* dst = (uint8_t*)bits;
    const uint8_t* src = (const uint8_t*)tmp_bits;
    for (int i = 0; i < w * h; ++i) {
        dst[i * 4 + 0] = src[i * 4 + 0];
        dst[i * 4 + 1] = src[i * 4 + 1];
        dst[i * 4 + 2] = src[i * 4 + 2];
        dst[i * 4 + 3] = 255;
    }
    SelectObject(dc, old);
    DeleteObject(tmp);
    DeleteDC(dc);
}
