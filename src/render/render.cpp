#include "render.h"
#include "abt_proto.h"
#include "abt_decode.h"
#include "abt_psd.h"
#include <stdlib.h>

// Implemented in abt_eps.c: Photoshop EPS hex resource block -> JPEG bytes (malloc'd).

namespace abt {

bool map_file(const wchar_t* path, MappedFile* mf) {
    memset(mf, 0, sizeof(*mf));
    mf->file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (mf->file == INVALID_HANDLE_VALUE) { mf->file = nullptr; return false; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(mf->file, &sz) || sz.QuadPart <= 0 || (uint64_t)sz.QuadPart > (uint64_t)SIZE_MAX) {
        CloseHandle(mf->file);
        mf->file = nullptr;
        return false;
    }
    mf->map = CreateFileMappingW(mf->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mf->map) { CloseHandle(mf->file); mf->file = nullptr; return false; }
    mf->data = (const uint8_t*)MapViewOfFile(mf->map, FILE_MAP_READ, 0, 0, 0);
    if (!mf->data) { CloseHandle(mf->map); CloseHandle(mf->file); memset(mf, 0, sizeof(*mf)); return false; }
    mf->size = (size_t)sz.QuadPart;
    return true;
}

void unmap_file(MappedFile* mf) {
    if (mf->data) UnmapViewOfFile(mf->data);
    if (mf->map) CloseHandle(mf->map);
    if (mf->file) CloseHandle(mf->file);
    memset(mf, 0, sizeof(*mf));
}

// Fit (w,h) into a (bw,bh) box. allow_up=false never enlarges.
static void fit_box(int w, int h, int bw, int bh, bool allow_up, int* dw, int* dh) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    double s = (double)bw / w;
    double s2 = (double)bh / h;
    if (s2 < s) s = s2;
    if (!allow_up && s > 1.0) s = 1.0;
    int x = (int)(w * s + 0.5), y = (int)(h * s + 0.5);
    *dw = x < 1 ? 1 : x;
    *dh = y < 1 ? 1 : y;
}

// True if the preview is at least as large as what the box needs (no enlargement).
static bool preview_adequate(const abt_preview& pv, int bw, int bh) {
    if (!pv.width || !pv.height || (pv.flags & ABT_PVF_ASPECT_MISMATCH)) return false;
    double s = (double)bw / pv.width, s2 = (double)bh / pv.height;
    if (s2 < s) s = s2;
    return s <= 1.0001;
}

static HRESULT fit_image(abt_image* img, int bw, int bh, bool allow_up) {
    int dw, dh;
    fit_box(img->w, img->h, bw, bh, allow_up, &dw, &dh);
    if (dw == img->w && dh == img->h) return S_OK;
    abt_image r;
    if (abt_resize(img->px, img->w, img->h, (size_t)img->w * 4, dw, dh, &r) != ABT_OK) return E_OUTOFMEMORY;
    abt_image_free(img);
    *img = r;
    abt_image_scan_alpha(img);
    return S_OK;
}

static HRESULT decode_preview(const MappedFile& mf, const abt_preview& pv, int bw, int bh, abt_image* out) {
    if (pv.offset > mf.size || pv.length > mf.size - pv.offset || pv.length == 0) return E_FAIL;
    const uint8_t* p = mf.data + pv.offset;
    size_t len = (size_t)pv.length;
    int edge = bw > bh ? bw : bh;
    HRESULT hr = E_FAIL;
    switch (pv.kind) {
    case ABT_PV_JPEG:
    case ABT_PV_PNG:
    case ABT_PV_TIFF:
    case ABT_PV_JPEG_BGR:
        if (pv.flags & ABT_PVF_BASE64) {
            uint8_t* buf = (uint8_t*)malloc(len / 4 * 3 + 4);
            if (!buf) return E_OUTOFMEMORY;
            size_t n = abt_base64_decode(p, len, buf);
            hr = decode_image_bytes(buf, n, edge, false, out);
            free(buf);
        } else if (pv.flags & ABT_PVF_HEX_PSRES) {
            uint8_t* jpeg = nullptr;
            size_t n = abt_eps_psres_jpeg(mf.data, mf.size, &pv, &jpeg);
            hr = n ? decode_image_bytes(jpeg, n, edge, false, out) : E_FAIL;
            free(jpeg);
        } else {
            hr = decode_image_bytes(p, len, edge, pv.kind == ABT_PV_JPEG_BGR, out);
        }
        break;
    case ABT_PV_WMF:
        hr = decode_wmf(p, len, edge, out);
        break;
    case ABT_PV_EPSI:
        hr = abt_epsi_decode(mf.data, mf.size, &pv, out) == ABT_OK ? S_OK : E_FAIL;
        break;
    default:
        break;
    }
    if (SUCCEEDED(hr)) hr = fit_image(out, bw, bh, false);
    return hr;
}

static bool g_background = false;

void render_init(bool background) { g_background = background; }

static void set_reason(RenderResult* rr, const char* s) { lstrcpynA(rr->reason, s, sizeof(rr->reason)); }

// Body of the pipeline; may fault on I/O errors of the mapped view (guarded by the caller).
static void render_inner(const RenderRequest& rq, const MappedFile& mf, RenderResult* rr) {
    abt_probe pr;
    abt_status st = abt_probe_file(mf.data, mf.size, &pr);
    rr->info = pr.info;
    if (st != ABT_OK) {
        rr->hr = HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
        _snprintf_s(rr->reason, sizeof(rr->reason), _TRUNCATE, "probe: %s", abt_status_str(st));
        return;
    }
    if (rq.mode == ABT_MODE_PROBE) { rr->hr = S_OK; set_reason(rr, "probe"); return; }

    abt_family fam = (abt_family)pr.info.family;
    int bw = rq.cx > 0 ? rq.cx : 256, bh = rq.cy > 0 ? rq.cy : bw;
    bool allow_embedded = !(rq.flags & ABT_RF_NO_EMBEDDED) && rq.mode != ABT_MODE_RENDER && rq.page == 0;
    int edge = bw > bh ? bw : bh;
    int best = abt_probe_best_preview(&pr, edge);

    // 1) An embedded preview that is big enough is exact and cheapest (D-011).
    if (allow_embedded && best >= 0 && preview_adequate(pr.previews[best], bw, bh)) {
        if (SUCCEEDED(decode_preview(mf, pr.previews[best], bw, bh, &rr->img))) {
            rr->hr = S_OK;
            rr->source = ABT_SRC_EMBEDDED;
            set_reason(rr, "embedded preview");
            return;
        }
    }
    if (rq.mode == ABT_MODE_FAST) {
        if (allow_embedded && best >= 0 && SUCCEEDED(decode_preview(mf, pr.previews[best], bw, bh, &rr->img))) {
            rr->hr = S_OK;
            rr->source = ABT_SRC_EMBEDDED | ABT_SRC_SCALED;
            set_reason(rr, "embedded preview (smaller than requested)");
        } else {
            rr->hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            set_reason(rr, "no embedded preview");
        }
        return;
    }

    // 2) Full render.
    HRESULT hr = E_NOTIMPL;
    switch (fam) {
    case ABT_FAM_AI_PDF:
    case ABT_FAM_PDF: {
        bool no_pdf = false;
        hr = render_pdf(mf.data, mf.size, rq, fam, &rr->img, &rr->info, &no_pdf);
        if (no_pdf) {
            rr->info.flags |= ABT_INFO_NO_PDF_CONTENT;
            set_reason(rr, "AI saved without PDF content");
            hr = E_NOTIMPL;
        } else if (SUCCEEDED(hr) && fam == ABT_FAM_AI_PDF) {
            rr->info.flags |= ABT_INFO_HAS_PDF_CONTENT;
        }
        break;
    }
    case ABT_FAM_PSD:
    case ABT_FAM_PSB: {
        bool approx = false;
        hr = render_psd(mf.data, mf.size, pr, rq, &rr->img, rr->reason, sizeof(rr->reason), &approx);
        // Our layer composite skips adjustment layers and effects, so its colours can be off.
        // Photoshop's own thumbnail (resource 1036, ~160 px) has the right colours: prefer it for
        // thumbnails it can serve with at most 2x enlargement (D-022).
        if (SUCCEEDED(hr) && approx && allow_embedded && best >= 0) {
            const abt_preview& pv = pr.previews[best];
            uint32_t pl = pv.width > pv.height ? pv.width : pv.height;
            abt_image em = {};
            if (pl && (uint32_t)edge <= pl * 2 && SUCCEEDED(decode_preview(mf, pv, bw, bh, &em))) {
                if (fit_image(&em, bw, bh, true) == S_OK) {
                    abt_image_free(&rr->img);
                    rr->img = em;
                    rr->hr = S_OK;
                    rr->source = ABT_SRC_EMBEDDED | ABT_SRC_SCALED;
                    set_reason(rr, "Photoshop thumbnail (layers use effects/adjustments)");
                    return;
                }
                abt_image_free(&em);
            }
        }
        if (SUCCEEDED(hr) && approx) set_reason(rr, "layer composite (adjustment layers/effects not applied)");
        break;
    }
    case ABT_FAM_EPS:
    case ABT_FAM_AI_PS:
        hr = render_ghostscript(rq.path, pr, rq, &rr->img);
        if (hr == E_NOTIMPL) set_reason(rr, "no PostScript renderer (Ghostscript not installed)");
        break;
    default:
        break;
    }
    if (SUCCEEDED(hr)) {
        rr->hr = S_OK;
        rr->source = ABT_SRC_RENDERED;
        if (!rr->reason[0]) set_reason(rr, "rendered");
        return;
    }

    // 3) Smaller embedded preview beats nothing.
    if (allow_embedded && best >= 0 && SUCCEEDED(decode_preview(mf, pr.previews[best], bw, bh, &rr->img))) {
        rr->hr = S_OK;
        rr->source = ABT_SRC_EMBEDDED | ABT_SRC_SCALED;
        if (!rr->reason[0]) set_reason(rr, "render failed, used smaller embedded preview");
        return;
    }
    rr->hr = FAILED(hr) ? hr : E_FAIL;
    if (!rr->reason[0]) _snprintf_s(rr->reason, sizeof(rr->reason), _TRUNCATE, "render failed 0x%08lx", hr);
}

static int seh_filter(DWORD code) {
    return (code == EXCEPTION_IN_PAGE_ERROR || code == EXCEPTION_ACCESS_VIOLATION) ? EXCEPTION_EXECUTE_HANDLER
                                                                                     : EXCEPTION_CONTINUE_SEARCH;
}

static bool render_guarded(const RenderRequest& rq, const MappedFile& mf, RenderResult* rr) {
    __try {
        render_inner(rq, mf, rr);
        return true;
    } __except (seh_filter(GetExceptionCode())) {
        return false;
    }
}

void render_file(const RenderRequest& rq, RenderResult* rr) {
    memset(rr, 0, sizeof(*rr));
    rr->hr = E_FAIL;
    uint64_t t0 = qpc();
    MappedFile mf;
    if (!map_file(rq.path, &mf)) {
        rr->hr = HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_OPEN_FAILED);
        set_reason(rr, "cannot open/map file");
    } else {
        if (!render_guarded(rq, mf, rr)) {
            abt_image_free(&rr->img);
            rr->hr = HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
            set_reason(rr, "I/O error or corrupt data while reading");
        }
        unmap_file(&mf);
    }
    if (FAILED(rr->hr)) abt_image_free(&rr->img);
    rr->elapsed_us = (uint32_t)(qpc_ms(t0, qpc()) * 1000.0);
}

void render_result_free(RenderResult* rr) { abt_image_free(&rr->img); }

// ------------------------------------------------------------------ PSD

HRESULT render_psd(const uint8_t* data, size_t size, const abt_probe& pr, const RenderRequest& rq, abt_image* out,
                   char* reason, size_t reason_cap, bool* approx) {
    *approx = false;
    int bw = rq.cx > 0 ? rq.cx : 256, bh = rq.cy > 0 ? rq.cy : bw;
    abt_psd_opts o = {};
    fit_box((int)pr.info.px_width, (int)pr.info.px_height, bw, bh, false, &o.dst_w, &o.dst_h);
    o.rows_per_out = (rq.flags & ABT_RF_MAX_QUALITY) ? 0 : 4;
    abt_pixbuf pb = {};
    abt_status st = abt_psd_decode_composite(data, size, &pr, &o, &pb);
    if (st == ABT_E_NOTFOUND) {
        st = abt_psd_composite_layers(data, size, &pr, &o, &pb);
        if (st == ABT_OK) lstrcpynA(reason, "composited from layers (no merged image)", (int)reason_cap);
        *approx = st == ABT_OK && pb.approx;
    }
    if (st != ABT_OK) {
        _snprintf_s(reason, reason_cap, _TRUNCATE, "psd: %s", abt_status_str(st));
        return E_FAIL;
    }
    CmykXform* xf = nullptr;
    if (pb.cs == ABT_CM_CMYK) {
        const uint8_t* icc = nullptr;
        size_t icc_len = 0;
        if (pr.psd_icc_length && pr.psd_icc_offset < size && pr.psd_icc_length <= size - pr.psd_icc_offset) {
            icc = data + pr.psd_icc_offset;
            icc_len = (size_t)pr.psd_icc_length;
        }
        xf = cmyk_xform_create(icc, icc_len);
    }
    st = abt_pixbuf_to_bgra(&pb, xf ? cmyk_xform_fn : nullptr, xf, out);
    cmyk_xform_destroy(xf);
    abt_pixbuf_free(&pb);
    return st == ABT_OK ? S_OK : E_FAIL;
}

}  // namespace abt
