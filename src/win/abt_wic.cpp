#include "abt_wic.h"
#include "abt_image.h"
#include "abt_proto.h"
#include <new>

namespace abt {

static SRWLOCK g_wic_lock = SRWLOCK_INIT;
static IWICImagingFactory* g_wic = nullptr;

// Reusable scratch buffer for BGRA -> 24bpp BGR conversion in encode_jpeg_bgra.
static SRWLOCK g_bgr_lock = SRWLOCK_INIT;
static uint8_t* g_bgr_buf = nullptr;
static size_t g_bgr_cap = 0;

static uint8_t* bgr_scratch_take(size_t need, size_t* out_cap) {
    uint8_t* p = nullptr;
    size_t cap = 0;
    AcquireSRWLockExclusive(&g_bgr_lock);
    if (g_bgr_buf) {
        p = g_bgr_buf;
        cap = g_bgr_cap;
        g_bgr_buf = nullptr;
        g_bgr_cap = 0;
    }
    ReleaseSRWLockExclusive(&g_bgr_lock);
    if (cap < need) {
        mem_free(p);
        p = (uint8_t*)mem_alloc(need);
        cap = p ? need : 0;
    }
    *out_cap = cap;
    return p;
}

static void bgr_scratch_return(uint8_t* p, size_t cap) {
    if (!p) return;
    if (cap > (16u << 20)) {  // keep cached buffer bounded
        mem_free(p);
        return;
    }
    AcquireSRWLockExclusive(&g_bgr_lock);
    if (!g_bgr_buf) {
        g_bgr_buf = p;
        g_bgr_cap = cap;
        p = nullptr;
    }
    ReleaseSRWLockExclusive(&g_bgr_lock);
    mem_free(p);
}

HRESULT wic_factory(IWICImagingFactory** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    AcquireSRWLockShared(&g_wic_lock);
    if (g_wic) {
        g_wic->AddRef();
        *out = g_wic;
        ReleaseSRWLockShared(&g_wic_lock);
        return S_OK;
    }
    ReleaseSRWLockShared(&g_wic_lock);

    IWICImagingFactory* f = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (FAILED(hr)) return hr;

    AcquireSRWLockExclusive(&g_wic_lock);
    if (!g_wic) {
        g_wic = f;
        g_wic->AddRef();
        *out = g_wic;
    } else {
        g_wic->AddRef();
        *out = g_wic;
        f->Release();
    }
    ReleaseSRWLockExclusive(&g_wic_lock);
    return S_OK;
}

void wic_shutdown() {
    IWICImagingFactory* f = nullptr;
    AcquireSRWLockExclusive(&g_wic_lock);
    f = g_wic;
    g_wic = nullptr;
    ReleaseSRWLockExclusive(&g_wic_lock);
    if (f) f->Release();

    uint8_t* p = nullptr;
    AcquireSRWLockExclusive(&g_bgr_lock);
    p = g_bgr_buf;
    g_bgr_buf = nullptr;
    g_bgr_cap = 0;
    ReleaseSRWLockExclusive(&g_bgr_lock);
    mem_free(p);
}

HBITMAP create_dib32(int w, int h, void** bits) {
    if (w <= 0 || h <= 0 || w > ABT_MAX_DIM || h > ABT_MAX_DIM) return nullptr;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
}

HRESULT save_png_bgra(const wchar_t* path, const uint8_t* px, int w, int h, int stride, bool premultiplied) {
    if (!path || !px || w <= 0 || h <= 0 || w > ABT_MAX_DIM || h > ABT_MAX_DIM) return E_INVALIDARG;
    if (stride < w * 4 || (uint64_t)stride * (uint64_t)h > ABT_MAX_PIXEL_BYTES) return E_INVALIDARG;
    const UINT buf_bytes = (UINT)((uint64_t)stride * (uint64_t)h);

    IWICImagingFactory* f = nullptr;
    HRESULT hr = wic_factory(&f);
    if (FAILED(hr)) return hr;
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    hr = f->CreateStream(&st);
    if (SUCCEEDED(hr)) hr = st->InitializeFromFilename(path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize((UINT)w, (UINT)h);
    WICPixelFormatGUID fmt = premultiplied ? GUID_WICPixelFormat32bppPBGRA : GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr)) {
        if (fmt == (premultiplied ? GUID_WICPixelFormat32bppPBGRA : GUID_WICPixelFormat32bppBGRA)) {
            hr = frame->WritePixels((UINT)h, (UINT)stride, buf_bytes, (BYTE*)px);
        } else {
            // Encoder picked another format: convert through a WIC bitmap.
            IWICBitmap* bmp = nullptr;
            IWICFormatConverter* conv = nullptr;
            hr = f->CreateBitmapFromMemory((UINT)w, (UINT)h,
                                           premultiplied ? GUID_WICPixelFormat32bppPBGRA : GUID_WICPixelFormat32bppBGRA,
                                           (UINT)stride, buf_bytes, (BYTE*)px, &bmp);
            if (SUCCEEDED(hr)) hr = f->CreateFormatConverter(&conv);
            if (SUCCEEDED(hr))
                hr = conv->Initialize(bmp, fmt, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
            if (SUCCEEDED(hr)) hr = frame->WriteSource(conv, nullptr);
            if (conv) conv->Release();
            if (bmp) bmp->Release();
        }
    }
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    f->Release();
    return hr;
}

// Lightweight in-memory IStream backed by a single growable mem_alloc buffer. Eliminates
// CreateStreamOnHGlobal / GlobalReAlloc / Stat / Seek / Read and the extra copy of the JPEG bytes.
struct MemWriteStream final : IStream {
    volatile LONG refs;
    uint8_t* buf;
    size_t cap, len, pos;

    explicit MemWriteStream(size_t init_cap) : refs(1), buf(nullptr), cap(0), len(0), pos(0) {
        if (init_cap < 4096) init_cap = 4096;
        buf = (uint8_t*)mem_alloc(init_cap);
        if (buf) cap = init_cap;
    }

    uint8_t* detach(size_t* out_len) {
        uint8_t* p = buf;
        *out_len = len;
        buf = nullptr;
        cap = len = pos = 0;
        return p;
    }

    bool grow(size_t need) {
        if (need <= cap) return true;
        if (need > ABT_MAX_PIXEL_BYTES) return false;
        size_t ncap = cap ? cap : 4096;
        while (ncap < need) {
            if (ncap > ABT_MAX_PIXEL_BYTES / 2) { ncap = ABT_MAX_PIXEL_BYTES; break; }
            ncap *= 2;
        }
        uint8_t* nb = buf ? (uint8_t*)HeapReAlloc(GetProcessHeap(), 0, buf, ncap) : (uint8_t*)mem_alloc(ncap);
        if (!nb) return false;
        buf = nb;
        cap = ncap;
        return true;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ISequentialStream || riid == IID_IStream) {
            *ppv = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = (ULONG)InterlockedDecrement(&refs);
        if (r == 0) {
            mem_free(buf);
            buf = nullptr;
            mem_free(this);
        }
        return r;
    }
    HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcbRead) override {
        if (pcbRead) *pcbRead = 0;
        if (!pv && cb) return STG_E_INVALIDPOINTER;
        size_t avail = pos < len ? len - pos : 0;
        size_t n = cb < avail ? cb : avail;
        if (n && buf) { memcpy(pv, buf + pos, n); pos += n; }
        if (pcbRead) *pcbRead = (ULONG)n;
        return n == cb ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* pv, ULONG cb, ULONG* pcbWritten) override {
        if (pcbWritten) *pcbWritten = 0;
        if (!pv && cb) return STG_E_INVALIDPOINTER;
        if (cb == 0) return S_OK;
        if (pos > ABT_MAX_PIXEL_BYTES || (size_t)cb > ABT_MAX_PIXEL_BYTES - pos) return STG_E_MEDIUMFULL;
        if (!grow(pos + cb) || !buf) return E_OUTOFMEMORY;
        memcpy(buf + pos, pv, cb);
        pos += cb;
        if (pos > len) len = pos;
        if (pcbWritten) *pcbWritten = cb;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER* plibNewPosition) override {
        int64_t base = 0;
        if (dwOrigin == STREAM_SEEK_SET) base = 0;
        else if (dwOrigin == STREAM_SEEK_CUR) base = (int64_t)pos;
        else if (dwOrigin == STREAM_SEEK_END) base = (int64_t)len;
        else return STG_E_INVALIDFUNCTION;
        int64_t np = base + dlibMove.QuadPart;
        if (np < 0 || (uint64_t)np > ABT_MAX_PIXEL_BYTES) return STG_E_INVALIDFUNCTION;
        pos = (size_t)np;
        if (plibNewPosition) plibNewPosition->QuadPart = (ULONGLONG)pos;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER libNewSize) override {
        if (libNewSize.QuadPart > ABT_MAX_PIXEL_BYTES) return STG_E_MEDIUMFULL;
        size_t n = (size_t)libNewSize.QuadPart;
        if (n > cap && !grow(n)) return E_OUTOFMEMORY;
        if (n > len && buf) memset(buf + len, 0, n - len);
        len = n;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* pstatstg, DWORD) override {
        if (!pstatstg) return STG_E_INVALIDPOINTER;
        memset(pstatstg, 0, sizeof(*pstatstg));
        pstatstg->type = STGTY_STREAM;
        pstatstg->cbSize.QuadPart = (ULONGLONG)len;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }
};

// Packs BGRA (4 bytes/px) into 24bpp BGR (3 bytes/px, 4-byte padded rows), 4 pixels per step.
static void bgra_to_bgr24(const uint8_t* src, int w, int h, uint8_t* dst, size_t dst_stride) {
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = src + (size_t)y * (size_t)w * 4;
        uint8_t* d = dst + (size_t)y * dst_stride;
        int x = 0;
        for (; x + 4 <= w; x += 4, s += 16, d += 12) {
            uint32_t p0, p1, p2, p3;
            memcpy(&p0, s + 0, 4);
            memcpy(&p1, s + 4, 4);
            memcpy(&p2, s + 8, 4);
            memcpy(&p3, s + 12, 4);
            uint32_t d0 = (p0 & 0x00FFFFFFu) | (p1 << 24);
            uint32_t d1 = ((p1 >> 8) & 0x0000FFFFu) | ((p2 & 0x0000FFFFu) << 16);
            uint32_t d2 = ((p2 >> 16) & 0x000000FFu) | ((p3 & 0x00FFFFFFu) << 8);
            memcpy(d + 0, &d0, 4);
            memcpy(d + 4, &d1, 4);
            memcpy(d + 8, &d2, 4);
        }
        for (; x < w; ++x, s += 4, d += 3) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
    }
}

HRESULT encode_jpeg_bgra(const uint8_t* px, int w, int h, float quality, uint8_t** out, size_t* len) {
    if (!out || !len) return E_POINTER;
    *out = nullptr;
    *len = 0;
    if (!px || w <= 0 || h <= 0 || w > ABT_MAX_DIM || h > ABT_MAX_DIM ||
        (uint64_t)w * (uint64_t)h * 4u > ABT_MAX_PIXEL_BYTES)
        return E_INVALIDARG;

    IWICImagingFactory* f = nullptr;
    HRESULT hr = wic_factory(&f);
    if (FAILED(hr)) return hr;

    size_t est = ((size_t)w * (size_t)h) / 4;
    if (est < 16384) est = 16384;
    if (est > (1u << 20)) est = 1u << 20;
    MemWriteStream* st = (MemWriteStream*)mem_alloc(sizeof(MemWriteStream));
    if (!st) { f->Release(); return E_OUTOFMEMORY; }
    new (st) MemWriteStream(est);
    if (!st->buf) { st->Release(); f->Release(); return E_OUTOFMEMORY; }

    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    hr = f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, &props);
    if (SUCCEEDED(hr) && props) {
        PROPBAG2 opt[2] = {};
        VARIANT val[2];
        opt[0].pstrName = (LPOLESTR)L"ImageQuality";
        VariantInit(&val[0]);
        val[0].vt = VT_R4;
        val[0].fltVal = quality;
        opt[1].pstrName = (LPOLESTR)L"JpegYCrCbSubsampling";
        VariantInit(&val[1]);
        val[1].vt = VT_UI1;
        val[1].bVal = WICJpegYCrCbSubsampling444;
        props->Write(2, opt, val);  // best effort: older WIC may not know the subsampling option
    }
    if (SUCCEEDED(hr)) hr = frame->Initialize(props);
    if (SUCCEEDED(hr)) hr = frame->SetSize((UINT)w, (UINT)h);

    if (SUCCEEDED(hr)) {
        WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
        hr = frame->SetPixelFormat(&pf);
        if (SUCCEEDED(hr) && pf == GUID_WICPixelFormat24bppBGR) {
            const size_t bgr_stride = ((size_t)w * 3 + 3) & ~(size_t)3;
            const size_t bgr_bytes = bgr_stride * (size_t)h;
            size_t bgr_cap = 0;
            uint8_t* bgr = bgr_scratch_take(bgr_bytes, &bgr_cap);
            if (!bgr) {
                hr = E_OUTOFMEMORY;
            } else {
                bgra_to_bgr24(px, w, h, bgr, bgr_stride);
                hr = frame->WritePixels((UINT)h, (UINT)bgr_stride, (UINT)bgr_bytes, bgr);
                bgr_scratch_return(bgr, bgr_cap);
            }
        } else if (SUCCEEDED(hr)) {
            IWICBitmap* bmp = nullptr;
            const UINT src_bytes = (UINT)((uint64_t)w * (uint64_t)h * 4u);
            hr = f->CreateBitmapFromMemory((UINT)w, (UINT)h, GUID_WICPixelFormat32bppBGR, (UINT)w * 4, src_bytes,
                                           (BYTE*)px, &bmp);
            if (SUCCEEDED(hr)) hr = frame->WriteSource(bmp, nullptr);
            if (bmp) bmp->Release();
        }
    }
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (SUCCEEDED(hr)) {
        if (st->len == 0 || st->len > ULONG_MAX) {
            hr = E_FAIL;
        } else {
            *out = st->detach(len);
        }
    }
    if (props) props->Release();
    if (frame) frame->Release();
    if (enc) enc->Release();
    st->Release();
    f->Release();
    return hr;
}

HRESULT hbitmap_to_bgra(HBITMAP bmp, uint8_t** px, int* w, int* h, bool shell_bitmap) {
    if (!bmp || !px || !w || !h) return E_INVALIDARG;
    *px = nullptr;
    DIBSECTION ds = {};
    if (GetObjectW(bmp, sizeof(ds), &ds) != sizeof(ds) || ds.dsBm.bmBitsPixel != 32 || !ds.dsBm.bmBits) {
        // Not a 32bpp DIB section: go through GetDIBits.
        BITMAP bm = {};
        if (!GetObjectW(bmp, sizeof(bm), &bm)) return E_INVALIDARG;
        if (bm.bmWidth <= 0 || bm.bmHeight <= 0 || bm.bmWidth > ABT_MAX_DIM || bm.bmHeight > ABT_MAX_DIM ||
            (uint64_t)bm.bmWidth * (uint64_t)bm.bmHeight * 4u > ABT_MAX_PIXEL_BYTES)
            return E_INVALIDARG;
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        uint8_t* out = (uint8_t*)mem_alloc((size_t)bm.bmWidth * (size_t)bm.bmHeight * 4);
        if (!out) return E_OUTOFMEMORY;
        HDC dc = GetDC(nullptr);
        if (!dc) { mem_free(out); return E_FAIL; }
        int r = GetDIBits(dc, bmp, 0, (UINT)bm.bmHeight, out, &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        if (!r) { mem_free(out); return E_FAIL; }
        *px = out; *w = bm.bmWidth; *h = bm.bmHeight;
        return S_OK;
    }
    int bw = ds.dsBm.bmWidth, bh = ds.dsBm.bmHeight;
    if (bw <= 0 || bh <= 0 || bw > ABT_MAX_DIM || bh > ABT_MAX_DIM ||
        (uint64_t)bw * (uint64_t)bh * 4u > ABT_MAX_PIXEL_BYTES || ds.dsBm.bmWidthBytes < bw * 4)
        return E_INVALIDARG;
    uint8_t* out = (uint8_t*)mem_alloc((size_t)bw * (size_t)bh * 4);
    if (!out) return E_OUTOFMEMORY;
    // Thumbnail-cache bitmaps (IThumbnailCache / IShellItemImageFactory) report a positive height
    // but hold their rows top-first; verified with Windows' own PNG thumbnails. Explorer draws
    // them that way, so read them in memory order.
    bool bottom_up = ds.dsBmih.biHeight > 0 && !shell_bitmap;
    for (int y = 0; y < bh; ++y) {
        const uint8_t* src = (const uint8_t*)ds.dsBm.bmBits + (size_t)(bottom_up ? bh - 1 - y : y) * ds.dsBm.bmWidthBytes;
        memcpy(out + (size_t)y * (size_t)bw * 4, src, (size_t)bw * 4);
    }
    *px = out; *w = bw; *h = bh;
    return S_OK;
}

}  // namespace abt
