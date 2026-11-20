#include "abt_decode.h"
#include "abt_wic.h"
#include "abt_reader.h"
#include "abt_proto.h"
#include <icm.h>
#include <stdlib.h>

namespace abt {

HRESULT decode_image_bytes(const uint8_t* data, size_t n, int edge, bool bgr_swap, abt_image* out) {
    out->px = nullptr;
    if (!data || n < 8 || n > 0xFFFFFFFFu) return E_INVALIDARG;
    IWICImagingFactory* f = nullptr;
    HRESULT hr = wic_factory(&f);
    if (FAILED(hr)) return hr;
    IWICStream* st = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapSource* src = nullptr;
    IWICFormatConverter* conv = nullptr;
    hr = f->CreateStream(&st);
    if (SUCCEEDED(hr)) hr = st->InitializeFromMemory((BYTE*)data, (DWORD)n);
    if (SUCCEEDED(hr)) hr = f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &frame);
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) hr = frame->GetSize(&w, &h);
    if (SUCCEEDED(hr) && (w == 0 || h == 0 || w > ABT_MAX_DIM || h > ABT_MAX_DIM)) hr = E_FAIL;
    if (SUCCEEDED(hr)) {
        src = frame;
        src->AddRef();
        // JPEG: let the decoder scale by 1/2, 1/4, 1/8 when the target is much smaller (much faster).
        int tw, th;
        abt_fit((int)w, (int)h, edge, 0, &tw, &th);
        IWICBitmapSourceTransform* xf = nullptr;
        if ((UINT)tw * 2 <= w && SUCCEEDED(frame->QueryInterface(IID_PPV_ARGS(&xf)))) {
            UINT sw = (UINT)tw, sh = (UINT)th;
            if (SUCCEEDED(xf->GetClosestSize(&sw, &sh)) && sw >= (UINT)tw && sh >= (UINT)th && sw < w) {
                IWICBitmapScaler* sc = nullptr;
                // A scaler over a frame that supports IWICBitmapSourceTransform uses the native scaled decode.
                if (SUCCEEDED(f->CreateBitmapScaler(&sc)) &&
                    SUCCEEDED(sc->Initialize(frame, sw, sh, WICBitmapInterpolationModeFant))) {
                    src->Release();
                    src = sc;
                    w = sw;
                    h = sh;
                } else if (sc) {
                    sc->Release();
                }
            }
            xf->Release();
        }
        hr = f->CreateFormatConverter(&conv);
        if (SUCCEEDED(hr))
            hr = conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                  WICBitmapPaletteTypeCustom);
    }
    uint8_t* px = nullptr;
    // A hostile embedded preview may claim up to 32768 x 32768 (w * h * 4 also wraps the UINT below).
    if (SUCCEEDED(hr) && (uint64_t)w * h * 4 > ABT_MAX_PIXEL_BYTES) hr = E_FAIL;
    if (SUCCEEDED(hr)) {
        px = (uint8_t*)malloc((size_t)w * h * 4);
        hr = px ? conv->CopyPixels(nullptr, w * 4, w * h * 4, px) : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) {
        if (bgr_swap)
            for (size_t i = 0; i < (size_t)w * h; ++i) { uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
        int dw, dh;
        abt_fit((int)w, (int)h, edge, 0, &dw, &dh);
        if (dw == (int)w && dh == (int)h) {
            out->px = px; out->w = (int)w; out->h = (int)h;
            px = nullptr;
        } else if (abt_resize(px, (int)w, (int)h, (size_t)w * 4, dw, dh, out) != ABT_OK) {
            hr = E_OUTOFMEMORY;
        }
        if (SUCCEEDED(hr)) abt_image_scan_alpha(out);
    }
    free(px);
    if (conv) conv->Release();
    if (src) src->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (st) st->Release();
    f->Release();
    return hr;
}

HRESULT decode_wmf(const uint8_t* data, size_t n, int edge, abt_image* out) {
    out->px = nullptr;
    if (n < 18 || n > (64u << 20)) return E_INVALIDARG;
    const uint8_t* bits = data;
    size_t blen = n;
    int pw = 0, ph = 0;
    if (abt_load_u32le(data) == 0x9AC6CDD7u && n > 22) {  // placeable header
        int16_t l = (int16_t)(data[6] | data[7] << 8), t = (int16_t)(data[8] | data[9] << 8);
        int16_t r = (int16_t)(data[10] | data[11] << 8), b = (int16_t)(data[12] | data[13] << 8);
        pw = abs(r - l);
        ph = abs(b - t);
        bits += 22;
        blen -= 22;
    }
    HENHMETAFILE emf = SetWinMetaFileBits((UINT)blen, bits, nullptr, nullptr);
    if (!emf) return E_FAIL;
    ENHMETAHEADER hdr = {};
    GetEnhMetaFileHeader(emf, sizeof(hdr), &hdr);
    if (!pw || !ph) {
        pw = hdr.rclBounds.right - hdr.rclBounds.left;
        ph = hdr.rclBounds.bottom - hdr.rclBounds.top;
    }
    if (pw <= 0 || ph <= 0) { pw = 256; ph = 256; }
    int dw, dh;
    abt_fit(pw, ph, edge, 1, &dw, &dh);
    void* dib = nullptr;
    HBITMAP bmp = create_dib32(dw, dh, &dib);
    HRESULT hr = E_FAIL;
    if (bmp) {
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ old = SelectObject(dc, bmp);
        RECT rc = {0, 0, dw, dh};
        FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        SetStretchBltMode(dc, HALFTONE);
        PlayEnhMetaFile(dc, emf, &rc);
        GdiFlush();
        if (abt_image_alloc(out, dw, dh) == ABT_OK) {
            memcpy(out->px, dib, (size_t)dw * dh * 4);
            for (int i = 0; i < dw * dh; ++i) out->px[i * 4 + 3] = 255;
            hr = S_OK;
        }
        SelectObject(dc, old);
        DeleteDC(dc);
        DeleteObject(bmp);
    }
    DeleteEnhMetaFile(emf);
    return hr;
}

// ------------------------------------------------------------------ CMYK via Windows ICM

struct CmykXform {
    HPROFILE src, dst;
    HTRANSFORM xf;
};

static HPROFILE open_profile_file(const wchar_t* name) {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    DWORD dir_bytes = sizeof(dir);
    if (!GetColorDirectoryW(nullptr, dir, &dir_bytes)) return nullptr;
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, name);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return nullptr;
    PROFILE p = {PROFILE_FILENAME, path, (DWORD)((lstrlenW(path) + 1) * sizeof(wchar_t))};
    return OpenColorProfileW(&p, PROFILE_READ, FILE_SHARE_READ, OPEN_EXISTING);
}

CmykXform* cmyk_xform_create(const uint8_t* icc, size_t icc_len) {
    HPROFILE src = nullptr;
    if (icc && icc_len > 128 && icc_len < (16u << 20)) {
        PROFILE p = {PROFILE_MEMBUFFER, (PVOID)icc, (DWORD)icc_len};
        src = OpenColorProfileW(&p, PROFILE_READ, FILE_SHARE_READ, OPEN_EXISTING);
    }
    // Photoshop's default CMYK working space is U.S. Web Coated (SWOP) v2.
    if (!src) src = open_profile_file(L"USWebCoatedSWOP.icc");
    if (!src) src = open_profile_file(L"RSWOP.icm");
    if (!src) return nullptr;
    wchar_t srgb_path[MAX_PATH];
    DWORD sz = sizeof(srgb_path);
    HPROFILE dst = nullptr;
    if (GetStandardColorSpaceProfileW(nullptr, LCS_sRGB, srgb_path, &sz)) {
        PROFILE p = {PROFILE_FILENAME, srgb_path, (DWORD)((lstrlenW(srgb_path) + 1) * sizeof(wchar_t))};
        dst = OpenColorProfileW(&p, PROFILE_READ, FILE_SHARE_READ, OPEN_EXISTING);
    }
    if (!dst) { CloseColorProfile(src); return nullptr; }
    HPROFILE profiles[2] = {src, dst};
    DWORD intents[1] = {INTENT_RELATIVE_COLORIMETRIC};
    HTRANSFORM xf = CreateMultiProfileTransform(profiles, 2, intents, 1, BEST_MODE | USE_RELATIVE_COLORIMETRIC, INDEX_DONT_CARE);
    if (!xf) { CloseColorProfile(src); CloseColorProfile(dst); return nullptr; }
    CmykXform* x = (CmykXform*)mem_zalloc(sizeof(CmykXform));
    if (!x) { DeleteColorTransform(xf); CloseColorProfile(src); CloseColorProfile(dst); return nullptr; }
    x->src = src; x->dst = dst; x->xf = xf;
    return x;
}

void cmyk_xform_destroy(CmykXform* x) {
    if (!x) return;
    DeleteColorTransform(x->xf);
    CloseColorProfile(x->src);
    CloseColorProfile(x->dst);
    mem_free(x);
}

extern "C" int cmyk_xform_fn(void* ctx, const uint8_t* cmyk, uint8_t* rgb, size_t n) {
    CmykXform* x = (CmykXform*)ctx;
    if (!x || n > 0x3FFFFFFF) return 0;  // n * 4 must fit the DWORD stride below
    // ICM's BM_CMYKQUADS is a little-endian DWORD 0xCCMMYYKK, i.e. bytes K,Y,M,C in memory, and
    // BM_RGBTRIPLETS is B,G,R in memory (both verified with pure ink patches: `abthumb cmyktest`).
    uint8_t* bgr = (uint8_t*)mem_alloc(n * 3);
    uint8_t* kymc = (uint8_t*)mem_alloc(n * 4);
    if (!bgr || !kymc) { mem_free(bgr); mem_free(kymc); return 0; }
    for (size_t i = 0; i < n; ++i) {
        kymc[i * 4 + 0] = cmyk[i * 4 + 3];
        kymc[i * 4 + 1] = cmyk[i * 4 + 2];
        kymc[i * 4 + 2] = cmyk[i * 4 + 1];
        kymc[i * 4 + 3] = cmyk[i * 4 + 0];
    }
    BOOL ok = TranslateBitmapBits(x->xf, kymc, BM_CMYKQUADS, (DWORD)n, 1, (DWORD)(n * 4), bgr, BM_RGBTRIPLETS,
                                  (DWORD)(n * 3), nullptr, 0);
    mem_free(kymc);
    if (ok) {
        for (size_t i = 0; i < n; ++i) {
            rgb[i * 3 + 0] = bgr[i * 3 + 2];
            rgb[i * 3 + 1] = bgr[i * 3 + 1];
            rgb[i * 3 + 2] = bgr[i * 3 + 0];
        }
    }
    mem_free(bgr);
    return ok ? 1 : 0;
}

}  // namespace abt
