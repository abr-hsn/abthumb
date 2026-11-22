#include "abt_decode.h"
#include "abt_wic.h"
#include "abt_reader.h"
#include "abt_proto.h"
#include <icm.h>
#include <intrin.h>
#include <stdlib.h>

namespace abt {

// Reusable scratch buffers for WIC 24bpp decode and temporary full-size BGRA before resize.
static SRWLOCK g_dec_lock = SRWLOCK_INIT;
static uint8_t* g_dec_buf = nullptr;
static size_t g_dec_cap = 0;

static uint8_t* dec_scratch_take(size_t need, size_t* out_cap) {
    uint8_t* p = nullptr;
    size_t cap = 0;
    AcquireSRWLockExclusive(&g_dec_lock);
    if (g_dec_buf) {
        p = g_dec_buf;
        cap = g_dec_cap;
        g_dec_buf = nullptr;
        g_dec_cap = 0;
    }
    ReleaseSRWLockExclusive(&g_dec_lock);
    if (cap < need) {
        free(p);
        p = (uint8_t*)malloc(need);
        cap = p ? need : 0;
    }
    *out_cap = cap;
    return p;
}

static void dec_scratch_return(uint8_t* p, size_t cap) {
    if (!p) return;
    if (cap > (16u << 20)) {
        free(p);
        return;
    }
    AcquireSRWLockExclusive(&g_dec_lock);
    if (!g_dec_buf) {
        g_dec_buf = p;
        g_dec_cap = cap;
        p = nullptr;
    }
    ReleaseSRWLockExclusive(&g_dec_lock);
    free(p);
}

static void fit_box_down(int sw, int sh, int max_w, int max_h, int* dw, int* dh) {
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    if (max_w < 1) max_w = 1;
    if (max_h < 1) max_h = 1;
    if (sw <= max_w && sh <= max_h) { *dw = sw; *dh = sh; return; }
    double s = (double)max_w / sw;
    double s2 = (double)max_h / sh;
    if (s2 < s) s = s2;
    int x = (int)(sw * s + 0.5), y = (int)(sh * s + 0.5);
    *dw = x < 1 ? 1 : (x > max_w ? max_w : x);
    *dh = y < 1 ? 1 : (y > max_h ? max_h : y);
}

// Fast 24bpp BGR -> 32bpp opaque BGRA (with optional R/B swap for PSD resource 1033).
static void bgr24_to_bgra(const uint8_t* src, size_t src_stride, int w, int h, bool bgr_swap, uint8_t* dst) {
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = src + (size_t)y * src_stride;
        uint8_t* d = dst + (size_t)y * (size_t)w * 4;
        if (!bgr_swap) {
            int x = 0;
            for (; x + 4 <= w; x += 4, s += 12, d += 16) {
                uint32_t w0, w1, w2;
                memcpy(&w0, s + 0, 4);
                memcpy(&w1, s + 4, 4);
                memcpy(&w2, s + 8, 4);
                uint32_t p0 = (w0 & 0x00FFFFFFu) | 0xFF000000u;
                uint32_t p1 = ((w0 >> 24) | (w1 << 8)) | 0xFF000000u;
                uint32_t p2 = ((w1 >> 16) | (w2 << 16)) | 0xFF000000u;
                uint32_t p3 = (w2 >> 8) | 0xFF000000u;
                memcpy(d + 0, &p0, 4);
                memcpy(d + 4, &p1, 4);
                memcpy(d + 8, &p2, 4);
                memcpy(d + 12, &p3, 4);
            }
            for (; x < w; ++x, s += 3, d += 4) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = 255;
            }
        } else {
            for (int x = 0; x < w; ++x, s += 3, d += 4) {
                d[0] = s[2];
                d[1] = s[1];
                d[2] = s[0];
                d[3] = 255;
            }
        }
    }
}

static bool is_opaque_wic_format(const WICPixelFormatGUID& pf) {
    return pf == GUID_WICPixelFormat24bppBGR || pf == GUID_WICPixelFormat24bppRGB ||
           pf == GUID_WICPixelFormat32bppBGR || pf == GUID_WICPixelFormat32bppRGB ||
           pf == GUID_WICPixelFormat8bppGray || pf == GUID_WICPixelFormat16bppGray ||
           pf == GUID_WICPixelFormat32bppCMYK || pf == GUID_WICPixelFormat48bppRGB;
}

HRESULT decode_image_bytes_box(const uint8_t* data, size_t n, int max_w, int max_h, bool bgr_swap, abt_image* out,
                               bool known_opaque) {
    if (!out) return E_POINTER;
    out->px = nullptr;
    out->w = out->h = out->has_alpha = 0;
    if (!data || n < 8 || n > 0xFFFFFFFFu || max_w <= 0 || max_h <= 0) return E_INVALIDARG;
    if (max_w > ABT_MAX_DIM) max_w = ABT_MAX_DIM;
    if (max_h > ABT_MAX_DIM) max_h = ABT_MAX_DIM;

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

    int tw = 0, th = 0;
    WICPixelFormatGUID pf = {};
    bool src_opaque = known_opaque;
    if (SUCCEEDED(hr)) {
        src = frame;
        src->AddRef();
        // JPEG: let the decoder scale by 1/2, 1/4, 1/8 when the target is much smaller (much faster).
        fit_box_down((int)w, (int)h, max_w, max_h, &tw, &th);
        IWICBitmapSourceTransform* xf = nullptr;
        if (((UINT)tw * 2 <= w || (UINT)th * 2 <= h) && SUCCEEDED(frame->QueryInterface(IID_PPV_ARGS(&xf)))) {
            UINT sw = (UINT)tw, sh = (UINT)th;
            if (SUCCEEDED(xf->GetClosestSize(&sw, &sh)) && sw >= (UINT)tw && sh >= (UINT)th && (sw < w || sh < h)) {
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
        if (SUCCEEDED(src->GetPixelFormat(&pf)) && is_opaque_wic_format(pf)) src_opaque = true;
    }

    // A hostile embedded preview may claim up to 32768 x 32768 (w * h * 4 also wraps UINT).
    if (SUCCEEDED(hr) && (uint64_t)w * (uint64_t)h * 4u > ABT_MAX_PIXEL_BYTES) hr = E_FAIL;

    if (SUCCEEDED(hr)) {
        fit_box_down((int)w, (int)h, max_w, max_h, &tw, &th);
        const bool exact = (tw == (int)w && th == (int)h);
        const size_t bgra_bytes = (size_t)w * (size_t)h * 4;

        if (pf == GUID_WICPixelFormat24bppBGR) {
            // Direct 24bpp BGR decode (JPEG fast path): skips IWICFormatConverter and alpha scan.
            const size_t bgr_stride = ((size_t)w * 3 + 3) & ~(size_t)3;
            const size_t bgr_bytes = bgr_stride * (size_t)h;
            size_t bgr_cap = 0;
            uint8_t* bgr = dec_scratch_take(bgr_bytes, &bgr_cap);
            hr = bgr ? src->CopyPixels(nullptr, (UINT)bgr_stride, (UINT)bgr_bytes, bgr) : E_OUTOFMEMORY;
            if (SUCCEEDED(hr)) {
                uint8_t* px = (uint8_t*)malloc(bgra_bytes);
                if (!px) {
                    hr = E_OUTOFMEMORY;
                } else {
                    bgr24_to_bgra(bgr, bgr_stride, (int)w, (int)h, bgr_swap, px);
                    if (exact) {
                        out->px = px;
                        out->w = (int)w;
                        out->h = (int)h;
                        out->has_alpha = 0;
                    } else {
                        if (abt_resize(px, (int)w, (int)h, (size_t)w * 4, tw, th, out) != ABT_OK)
                            hr = E_OUTOFMEMORY;
                        else
                            out->has_alpha = 0;
                        free(px);
                    }
                }
            }
            dec_scratch_return(bgr, bgr_cap);
        } else {
            IWICBitmapSource* pbgra_src = src;
            if (pf != GUID_WICPixelFormat32bppPBGRA) {
                hr = f->CreateFormatConverter(&conv);
                if (SUCCEEDED(hr))
                    hr = conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                          WICBitmapPaletteTypeCustom);
                pbgra_src = conv;
            }
            if (SUCCEEDED(hr)) {
                uint8_t* px = nullptr;
                size_t px_cap = 0;
                if (exact) px = (uint8_t*)malloc(bgra_bytes);
                else px = dec_scratch_take(bgra_bytes, &px_cap);
                hr = px ? pbgra_src->CopyPixels(nullptr, w * 4, (UINT)bgra_bytes, px) : E_OUTOFMEMORY;
                if (SUCCEEDED(hr)) {
                    if (bgr_swap) {
                        for (size_t off = 0; off + 3 < bgra_bytes; off += 4) {
                            uint8_t t = px[off];
                            px[off] = px[off + 2];
                            px[off + 2] = t;
                        }
                    }
                    if (exact) {
                        out->px = px;
                        out->w = (int)w;
                        out->h = (int)h;
                        px = nullptr;
                    } else if (abt_resize(px, (int)w, (int)h, (size_t)w * 4, tw, th, out) != ABT_OK) {
                        hr = E_OUTOFMEMORY;
                    }
                    if (SUCCEEDED(hr)) {
                        if (src_opaque) out->has_alpha = 0;
                        else abt_image_scan_alpha(out);
                    }
                }
                if (exact) free(px);
                else dec_scratch_return(px, px_cap);
            }
        }
    }

    if (conv) conv->Release();
    if (src) src->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (st) st->Release();
    f->Release();
    return hr;
}

HRESULT decode_image_bytes(const uint8_t* data, size_t n, int edge, bool bgr_swap, abt_image* out) {
    return decode_image_bytes_box(data, n, edge, edge, bgr_swap, out, false);
}

HRESULT decode_wmf(const uint8_t* data, size_t n, int edge, abt_image* out) {
    if (!out) return E_POINTER;
    out->px = nullptr;
    out->w = out->h = out->has_alpha = 0;
    if (!data || n < 18 || n > (64u << 20) || edge <= 0) return E_INVALIDARG;
    if (edge > ABT_MAX_DIM) edge = ABT_MAX_DIM;

    const uint8_t* bits = data;
    size_t blen = n;
    int pw = 0, ph = 0;
    if (abt_load_u32le(data) == 0x9AC6CDD7u && n > 22) {  // placeable header
        int16_t l = (int16_t)(data[6] | data[7] << 8), t = (int16_t)(data[8] | data[9] << 8);
        int16_t r = (int16_t)(data[10] | data[11] << 8), b = (int16_t)(data[12] | data[13] << 8);
        pw = abs((int)r - (int)l);
        ph = abs((int)b - (int)t);
        bits += 22;
        blen -= 22;
    }
    HENHMETAFILE emf = SetWinMetaFileBits((UINT)blen, bits, nullptr, nullptr);
    if (!emf) return E_FAIL;
    ENHMETAHEADER hdr = {};
    if (!pw || !ph) {
        if (GetEnhMetaFileHeader(emf, sizeof(hdr), &hdr)) {
            int64_t rw = (int64_t)hdr.rclBounds.right - (int64_t)hdr.rclBounds.left;
            int64_t rh = (int64_t)hdr.rclBounds.bottom - (int64_t)hdr.rclBounds.top;
            if (rw > 0 && rh > 0) {
                pw = rw > ABT_MAX_DIM ? ABT_MAX_DIM : (int)rw;
                ph = rh > ABT_MAX_DIM ? ABT_MAX_DIM : (int)rh;
            }
        }
    }
    if (pw <= 0 || ph <= 0) { pw = 256; ph = 256; }
    int dw, dh;
    abt_fit(pw, ph, edge, 1, &dw, &dh);
    if (dw <= 0 || dh <= 0 || dw > ABT_MAX_DIM || dh > ABT_MAX_DIM ||
        (uint64_t)dw * (uint64_t)dh * 4u > ABT_MAX_PIXEL_BYTES) {
        DeleteEnhMetaFile(emf);
        return E_FAIL;
    }
    void* dib = nullptr;
    HBITMAP bmp = create_dib32(dw, dh, &dib);
    HRESULT hr = E_FAIL;
    if (bmp) {
        HDC dc = CreateCompatibleDC(nullptr);
        if (dc) {
            HGDIOBJ old = SelectObject(dc, bmp);
            RECT rc = {0, 0, dw, dh};
            FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
            SetStretchBltMode(dc, HALFTONE);
            BOOL played = PlayEnhMetaFile(dc, emf, &rc);
            GdiFlush();
            if (played && dib && abt_image_alloc(out, dw, dh) == ABT_OK && out->px) {
                const size_t npx = (size_t)dw * (size_t)dh;
                memcpy(out->px, dib, npx * 4);
                for (size_t i = 0; i < npx; ++i) out->px[i * 4 + 3] = 255;
                out->has_alpha = 0;
                hr = S_OK;
            }
            SelectObject(dc, old);
            DeleteDC(dc);
        }
        DeleteObject(bmp);
    }
    DeleteEnhMetaFile(emf);
    return hr;
}

// ------------------------------------------------------------------ CMYK via Windows ICM (LRU cached)

struct CmykXform {
    HPROFILE src, dst;
    HTRANSFORM xf;
    bool cached;
    int slot;
};

static constexpr int kCmykCacheCap = 8;

struct CmykCacheSlot {
    CmykXform x;
    uint64_t hash;
    size_t icc_len;
    uint8_t* icc_copy;
    uint64_t last_use;
    bool used;
    bool in_use;
};

static SRWLOCK g_cmyk_lock = SRWLOCK_INIT;
static HPROFILE g_srgb_profile = nullptr;
static CmykCacheSlot g_cmyk_cache[kCmykCacheCap];
static uint64_t g_cmyk_clock = 0;

static uint64_t fnv1a_bytes(const uint8_t* p, size_t n) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static HPROFILE open_profile_file(const wchar_t* name) {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    // GetColorDirectoryW is deprecated (C4996); a result longer than the buffer is a failure too.
    DWORD n = ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\spool\\drivers\\color", dir, MAX_PATH);
    if (!n || n > MAX_PATH) return nullptr;
    if (_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, name) < 0) return nullptr;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return nullptr;
    PROFILE p = {PROFILE_FILENAME, path, (DWORD)((lstrlenW(path) + 1) * sizeof(wchar_t))};
    return OpenColorProfileW(&p, PROFILE_READ, FILE_SHARE_READ, OPEN_EXISTING);
}

static HPROFILE get_srgb_profile() {
    AcquireSRWLockShared(&g_cmyk_lock);
    HPROFILE p = g_srgb_profile;
    ReleaseSRWLockShared(&g_cmyk_lock);
    if (p) return p;

    wchar_t srgb_path[MAX_PATH];
    DWORD sz = sizeof(srgb_path);
    HPROFILE opened = nullptr;
    if (GetStandardColorSpaceProfileW(nullptr, LCS_sRGB, srgb_path, &sz)) {
        PROFILE prof = {PROFILE_FILENAME, srgb_path, (DWORD)((lstrlenW(srgb_path) + 1) * sizeof(wchar_t))};
        opened = OpenColorProfileW(&prof, PROFILE_READ, FILE_SHARE_READ, OPEN_EXISTING);
    }
    if (!opened) return nullptr;

    AcquireSRWLockExclusive(&g_cmyk_lock);
    if (!g_srgb_profile) {
        g_srgb_profile = opened;
        p = opened;
        opened = nullptr;
    } else {
        p = g_srgb_profile;
    }
    ReleaseSRWLockExclusive(&g_cmyk_lock);
    if (opened) CloseColorProfile(opened);
    return p;
}

CmykXform* cmyk_xform_create(const uint8_t* icc, size_t icc_len) {
    const bool use_icc = icc && icc_len > 128 && icc_len < (16u << 20);
    const size_t key_len = use_icc ? icc_len : 0;
    const uint64_t key_hash = use_icc ? fnv1a_bytes(icc, icc_len) : 0;

    // 1) Fast path: hit in 8-entry LRU cache.
    AcquireSRWLockExclusive(&g_cmyk_lock);
    for (int i = 0; i < kCmykCacheCap; ++i) {
        CmykCacheSlot& s = g_cmyk_cache[i];
        if (!s.used || s.in_use || s.icc_len != key_len || s.hash != key_hash) continue;
        if (key_len == 0 || (s.icc_copy && memcmp(s.icc_copy, icc, key_len) == 0)) {
            s.in_use = true;
            s.last_use = ++g_cmyk_clock;
            ReleaseSRWLockExclusive(&g_cmyk_lock);
            return &s.x;
        }
    }
    ReleaseSRWLockExclusive(&g_cmyk_lock);

    // 2) Cache miss: open source profile & build ICM transform outside the lock.
    HPROFILE dst = get_srgb_profile();
    if (!dst) return nullptr;

    HPROFILE src = nullptr;
    bool actually_embedded = false;
    if (use_icc) {
        PROFILE p = {PROFILE_MEMBUFFER, (PVOID)icc, (DWORD)icc_len};
        src = OpenColorProfileW(&p, PROFILE_READ, FILE_SHARE_READ, OPEN_EXISTING);
        actually_embedded = src != nullptr;
    }
    // Photoshop's default CMYK working space is U.S. Web Coated (SWOP) v2.
    if (!src) src = open_profile_file(L"USWebCoatedSWOP.icc");
    if (!src) src = open_profile_file(L"RSWOP.icm");
    if (!src) return nullptr;

    HPROFILE profiles[2] = {src, dst};
    DWORD intents[1] = {INTENT_RELATIVE_COLORIMETRIC};
    HTRANSFORM xf = CreateMultiProfileTransform(profiles, 2, intents, 1, BEST_MODE | USE_RELATIVE_COLORIMETRIC, INDEX_DONT_CARE);
    if (!xf) { CloseColorProfile(src); return nullptr; }

    // 3) Store in LRU cache if a free or idle slot is available.
    const size_t store_len = actually_embedded ? key_len : 0;
    const uint64_t store_hash = actually_embedded ? key_hash : 0;
    uint8_t* copy = nullptr;
    if (store_len) {
        copy = (uint8_t*)mem_alloc(store_len);
        if (copy) memcpy(copy, icc, store_len);
    }

    HTRANSFORM evict_xf = nullptr;
    HPROFILE evict_src = nullptr;
    uint8_t* evict_copy = nullptr;
    CmykXform* ret = nullptr;

    if (!store_len || copy) {
        AcquireSRWLockExclusive(&g_cmyk_lock);
        int victim = -1;
        for (int i = 0; i < kCmykCacheCap; ++i) {
            if (!g_cmyk_cache[i].used) { victim = i; break; }
            if (!g_cmyk_cache[i].in_use &&
                (victim < 0 || g_cmyk_cache[i].last_use < g_cmyk_cache[victim].last_use)) {
                victim = i;
            }
        }
        if (victim >= 0) {
            CmykCacheSlot& s = g_cmyk_cache[victim];
            if (s.used) {
                evict_xf = s.x.xf;
                evict_src = s.x.src;
                evict_copy = s.icc_copy;
            }
            s.x.src = src;
            s.x.dst = dst;
            s.x.xf = xf;
            s.x.cached = true;
            s.x.slot = victim;
            s.hash = store_hash;
            s.icc_len = store_len;
            s.icc_copy = copy;
            s.last_use = ++g_cmyk_clock;
            s.used = true;
            s.in_use = true;
            copy = nullptr;
            ret = &s.x;
        }
        ReleaseSRWLockExclusive(&g_cmyk_lock);
    }
    mem_free(copy);
    if (evict_xf) DeleteColorTransform(evict_xf);
    if (evict_src) CloseColorProfile(evict_src);
    mem_free(evict_copy);
    if (ret) return ret;

    // Fallback: all cache slots concurrently in use — return uncached instance.
    CmykXform* x = (CmykXform*)mem_zalloc(sizeof(CmykXform));
    if (!x) { DeleteColorTransform(xf); CloseColorProfile(src); return nullptr; }
    x->src = src;
    x->dst = dst;
    x->xf = xf;
    x->cached = false;
    x->slot = -1;
    return x;
}

void cmyk_xform_destroy(CmykXform* x) {
    if (!x) return;
    if (x->cached) {
        AcquireSRWLockExclusive(&g_cmyk_lock);
        if (x->slot >= 0 && x->slot < kCmykCacheCap) g_cmyk_cache[x->slot].in_use = false;
        ReleaseSRWLockExclusive(&g_cmyk_lock);
        return;
    }
    DeleteColorTransform(x->xf);
    CloseColorProfile(x->src);
    mem_free(x);
}

void cmyk_xform_shutdown() {
    AcquireSRWLockExclusive(&g_cmyk_lock);
    for (int i = 0; i < kCmykCacheCap; ++i) {
        CmykCacheSlot& s = g_cmyk_cache[i];
        if (!s.used) continue;
        if (s.x.xf) DeleteColorTransform(s.x.xf);
        if (s.x.src) CloseColorProfile(s.x.src);
        mem_free(s.icc_copy);
        memset(&s, 0, sizeof(s));
    }
    if (g_srgb_profile) {
        CloseColorProfile(g_srgb_profile);
        g_srgb_profile = nullptr;
    }
    ReleaseSRWLockExclusive(&g_cmyk_lock);

    uint8_t* p = nullptr;
    AcquireSRWLockExclusive(&g_dec_lock);
    p = g_dec_buf;
    g_dec_buf = nullptr;
    g_dec_cap = 0;
    ReleaseSRWLockExclusive(&g_dec_lock);
    free(p);
}

extern "C" int cmyk_xform_fn(void* ctx, const uint8_t* cmyk, uint8_t* rgb, size_t n) {
    if (n == 0) return 1;
    CmykXform* x = (CmykXform*)ctx;
    if (!x || !cmyk || !rgb || n > 0x3FFFFFFF) return 0;  // n * 4 must fit the DWORD stride below

    // Process in L1-friendly stack chunks (0 heap allocations) with 32-bit BSWAP CMYK -> KYMC.
    // ICM's BM_CMYKQUADS is a little-endian DWORD 0xCCMMYYKK (bytes K,Y,M,C) and BM_RGBTRIPLETS is B,G,R.
    constexpr size_t kChunk = 2048;
    uint8_t kymc[kChunk * 4];
    uint8_t bgr[kChunk * 3];

    for (size_t base = 0; base < n; base += kChunk) {
        const size_t cnt = (n - base) < kChunk ? (n - base) : kChunk;
        const uint8_t* sp = cmyk + base * 4;
        for (size_t i = 0; i < cnt; ++i) {
            unsigned long v;
            memcpy(&v, sp + i * 4, 4);
            v = _byteswap_ulong(v);
            memcpy(kymc + i * 4, &v, 4);
        }
        if (!TranslateBitmapBits(x->xf, kymc, BM_CMYKQUADS, (DWORD)cnt, 1, (DWORD)(cnt * 4), bgr, BM_RGBTRIPLETS,
                                 (DWORD)(cnt * 3), nullptr, 0))
            return 0;
        uint8_t* dp = rgb + base * 3;
        for (size_t i = 0; i < cnt; ++i) {
            dp[i * 3 + 0] = bgr[i * 3 + 2];
            dp[i * 3 + 1] = bgr[i * 3 + 1];
            dp[i * 3 + 2] = bgr[i * 3 + 0];
        }
    }
    return 1;
}

}  // namespace abt
