#include "abt_wic.h"

namespace abt {

static IWICImagingFactory* g_wic = nullptr;
static INIT_ONCE g_wic_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK wic_init(PINIT_ONCE, PVOID, PVOID*) {
    IWICImagingFactory* f = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))))
        g_wic = f;
    return TRUE;
}

HRESULT wic_factory(IWICImagingFactory** out) {
    InitOnceExecuteOnce(&g_wic_once, wic_init, nullptr, nullptr);
    if (!g_wic) return E_FAIL;
    g_wic->AddRef();
    *out = g_wic;
    return S_OK;
}

HBITMAP create_dib32(int w, int h, void** bits) {
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
            hr = frame->WritePixels((UINT)h, (UINT)stride, (UINT)(stride * h), (BYTE*)px);
        } else {
            // Encoder picked another format: convert through a WIC bitmap.
            IWICBitmap* bmp = nullptr;
            IWICFormatConverter* conv = nullptr;
            hr = f->CreateBitmapFromMemory((UINT)w, (UINT)h,
                                           premultiplied ? GUID_WICPixelFormat32bppPBGRA : GUID_WICPixelFormat32bppBGRA,
                                           (UINT)stride, (UINT)(stride * h), (BYTE*)px, &bmp);
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

HRESULT hbitmap_to_bgra(HBITMAP bmp, uint8_t** px, int* w, int* h, bool shell_bitmap) {
    DIBSECTION ds = {};
    if (GetObjectW(bmp, sizeof(ds), &ds) != sizeof(ds) || ds.dsBm.bmBitsPixel != 32 || !ds.dsBm.bmBits) {
        // Not a 32bpp DIB section: go through GetDIBits.
        BITMAP bm = {};
        if (!GetObjectW(bmp, sizeof(bm), &bm)) return E_INVALIDARG;
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        uint8_t* out = (uint8_t*)mem_alloc((size_t)bm.bmWidth * bm.bmHeight * 4);
        if (!out) return E_OUTOFMEMORY;
        HDC dc = GetDC(nullptr);
        int r = GetDIBits(dc, bmp, 0, (UINT)bm.bmHeight, out, &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        if (!r) { mem_free(out); return E_FAIL; }
        *px = out; *w = bm.bmWidth; *h = bm.bmHeight;
        return S_OK;
    }
    int bw = ds.dsBm.bmWidth, bh = ds.dsBm.bmHeight;
    uint8_t* out = (uint8_t*)mem_alloc((size_t)bw * bh * 4);
    if (!out) return E_OUTOFMEMORY;
    // Thumbnail-cache bitmaps (IThumbnailCache / IShellItemImageFactory) report a positive height
    // but hold their rows top-first; verified with Windows' own PNG thumbnails. Explorer draws
    // them that way, so read them in memory order.
    bool bottom_up = ds.dsBmih.biHeight > 0 && !shell_bitmap;
    for (int y = 0; y < bh; ++y) {
        const uint8_t* src = (const uint8_t*)ds.dsBm.bmBits + (size_t)(bottom_up ? bh - 1 - y : y) * ds.dsBm.bmWidthBytes;
        memcpy(out + (size_t)y * bw * 4, src, (size_t)bw * 4);
    }
    *px = out; *w = bw; *h = bh;
    return S_OK;
}

}  // namespace abt
