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
    if (mf->file == INVALID_HANDLE_VALUE) { mf->file = nullptr; mf->err = GetLastError(); return false; }
    LARGE_INTEGER sz;
    sz.QuadPart = -1;
    if (!GetFileSizeEx(mf->file, &sz) || sz.QuadPart <= 0 || (uint64_t)sz.QuadPart > (uint64_t)SIZE_MAX) {
        DWORD e = GetLastError();
        CloseHandle(mf->file);
        mf->file = nullptr;
        mf->err = sz.QuadPart == 0 ? ERROR_EMPTY : (e ? e : ERROR_FILE_TOO_LARGE);
        return false;
    }
    mf->map = CreateFileMappingW(mf->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mf->map) { mf->err = GetLastError(); CloseHandle(mf->file); mf->file = nullptr; return false; }
    mf->data = (const uint8_t*)MapViewOfFile(mf->map, FILE_MAP_READ, 0, 0, 0);
    if (!mf->data) {
        DWORD e = GetLastError();
        CloseHandle(mf->map);
        CloseHandle(mf->file);
        memset(mf, 0, sizeof(*mf));
        mf->err = e;
        return false;
    }
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
    // ABT_PVF_ASPECT_MISMATCH (Illustrator's thumbnail shows every artboard) is deliberately not a
    // reason to render: that thumbnail is the document thumbnail Illustrator and Bridge show, it is
    // instant, and real multi-artboard files can take seconds to render (D-020, revised).
    if (!pv.width || !pv.height) return false;
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
    // A blank preview (Illustrator 2026 sometimes writes an all-white 256 x 256 XMP thumbnail) is no
    // picture of the file: report failure so the pipeline renders instead.
    if (SUCCEEDED(hr) && abt_image_is_uniform(out, 6)) {
        abt_image_free(out);
        hr = HRESULT_FROM_WIN32(ERROR_EMPTY);
    }
    if (SUCCEEDED(hr)) hr = fit_image(out, bw, bh, false);
    return hr;
}

static void set_reason(RenderResult* rr, const char* s) { lstrcpynA(rr->reason, s, sizeof(rr->reason)); }

// Illustrator: is the whole canvas (all art, on and off the artboards) just artboard 1? Only then
// can PDF page 1 stand in for it; Illustrator's PDF pages hold each artboard's art clipped to that
// artboard and nothing outside them (D-029). The header's art bounds are in Illustrator's ruler space,
// whose origin is not artboard 1's corner in every file (D-032), so only their size is compared:
// art that fits in the artboard's size is taken to be on it. Art bounds are visual bounds, so a stroke
// that pokes a little past the artboard edge still counts as inside.
static bool ai_canvas_is_page1(const abt_probe& pr, int pages, float w0, float h0) {
    if (pages != 1) return false;
    const float* b = pr.bbox;
    if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0) return true;  // bounds unknown: single artboard
    float tol = 4.0f + 0.02f * (w0 > h0 ? w0 : h0);
    return b[2] - b[0] <= w0 + tol && b[3] - b[1] <= h0 + tol;
}

// The whole-canvas thumbnail of a multi-artboard Illustrator file: Illustrator's own document
// thumbnail, enlarged when the request is bigger than it (soft, but it is the only image that holds
// art outside the artboards).
static bool ai_canvas_thumbnail(const MappedFile& mf, const abt_probe& pr, int bw, int bh, RenderResult* rr) {
    int edge = bw > bh ? bw : bh;
    int best = -1;
    for (int i = 0; i < pr.n_previews; ++i) {
        if (pr.previews[i].source != ABT_PVS_XMP) continue;
        if (best < 0 || abt_probe_best_preview(&pr, edge) == i) best = i;
    }
    if (best < 0) return false;
    abt_image img = {};
    if (FAILED(decode_preview(mf, pr.previews[best], bw, bh, &img))) return false;
    bool small = img.w < bw && img.h < bh;
    if (small && fit_image(&img, bw, bh, true) != S_OK) { abt_image_free(&img); return false; }
    rr->img = img;
    rr->hr = S_OK;
    rr->source = ABT_SRC_EMBEDDED | (small ? ABT_SRC_SCALED : 0);
    set_reason(rr, "Illustrator document thumbnail (whole canvas)");
    return true;
}

// Share of pixels that are clearly not white (any channel below 230): how much art a render shows.
static double ink_fraction(const abt_image& img) {
    if (!img.px || img.w < 1 || img.h < 1) return 0.0;
    size_t n = (size_t)img.w * img.h, ink = 0;
    const uint8_t* p = img.px;
    for (size_t i = 0; i < n; ++i, p += 4)
        if (p[0] < 230 || p[1] < 230 || p[2] < 230) ++ink;
    return (double)ink / (double)n;
}

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
    abt_family fam = (abt_family)pr.info.family;
    const bool canvas = (uint32_t)rq.page == ABT_PAGE_CANVAS;
    const int page = canvas ? 0 : rq.page;

    // Illustrator: artboard count and whether the whole canvas is more than artboard 1 (D-029).
    bool ai_multi = false;
    if (fam == ABT_FAM_AI_PDF) {
        int pages = 0;
        float w0 = 0, h0 = 0;
        if (pdf_layout(mf.data, mf.size, &pages, &w0, &h0)) {
            rr->info.page_count = (uint32_t)pages;
            rr->info.pt_width = w0;  // stDim in the XMP is the active artboard, not artboard 1
            rr->info.pt_height = h0;
            ai_multi = !ai_canvas_is_page1(pr, pages, w0, h0);
            if (ai_multi) rr->info.flags |= ABT_INFO_CANVAS_BEYOND_PAGE1;
        }
    }
    if (rq.mode == ABT_MODE_PROBE) { rr->hr = S_OK; set_reason(rr, "probe"); return; }
    // Illustrator 8 and older (PostScript-based .ai) are out of scope (D-026): fail fast so
    // Explorer shows its icon; the failure is negative-cached and never retried.
    if (fam == ABT_FAM_AI_PS) {
        rr->hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        set_reason(rr, "Illustrator 8 and older files are not supported");
        return;
    }
    int bw = rq.cx > 0 ? rq.cx : 256, bh = rq.cy > 0 ? rq.cy : bw;
    bool allow_embedded = !(rq.flags & ABT_RF_NO_EMBEDDED) && rq.mode != ABT_MODE_RENDER && page == 0;
    int edge = bw > bh ? bw : bh;
    int best = abt_probe_best_preview(&pr, edge);

    if (ai_multi) {
        // Whole canvas: only Illustrator's document thumbnail has it (D-029).
        if (canvas && allow_embedded && ai_canvas_thumbnail(mf, pr, bw, bh, rr)) return;
        // One artboard: that thumbnail shows more than the artboard, so it never stands in for it.
        allow_embedded = false;
        if (canvas) set_reason(rr, "no document thumbnail: artboard 1 only");
    }

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
            // Illustrator's thumbnail is then the only picture of the file, canvas or not.
            allow_embedded = page == 0 && !(rq.flags & ABT_RF_NO_EMBEDDED) && rq.mode != ABT_MODE_RENDER;
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
        hr = render_ghostscript(rq.path, pr, rq, &rr->img);
        if (hr == E_NOTIMPL) set_reason(rr, "no PostScript renderer (Ghostscript not installed)");
        break;
    default:
        break;
    }
    // Artboard 1 has nothing visible on it but the art bounds said it held everything (the art sits off
    // the artboard): Illustrator's document thumbnail, when it is not blank too, is the better picture.
    if (SUCCEEDED(hr) && canvas && fam == ABT_FAM_AI_PDF && (rr->info.flags & ABT_INFO_PAGE_EMPTY) &&
        !(rq.flags & ABT_RF_NO_EMBEDDED) && rq.mode != ABT_MODE_RENDER) {
        abt_image page1 = rr->img;
        rr->img = {};
        if (ai_canvas_thumbnail(mf, pr, bw, bh, rr)) {
            abt_image_free(&page1);
            rr->info.flags &= ~(uint32_t)ABT_INFO_PAGE_EMPTY;
            return;
        }
        rr->img = page1;
    }
    // Still nothing visible (artboard 1 is empty, the document thumbnail blank or missing): a thumbnail
    // is never left white while another page or artboard has art, so show the first one that does.
    // An Illustrator artboard with only crop marks or a stray line counts as nearly empty (under 1 %
    // ink). PDFs keep a sparse page 1 (a cover is page 1 even when it is mostly white).
    if (SUCCEEDED(hr) && canvas && (fam == ABT_FAM_AI_PDF || fam == ABT_FAM_PDF) && rr->info.page_count > 1 &&
        ((rr->info.flags & ABT_INFO_PAGE_EMPTY) || (fam == ABT_FAM_AI_PDF && ink_fraction(rr->img) < 0.01))) {
        double best_ink = (rr->info.flags & ABT_INFO_PAGE_EMPTY) ? 0.0 : ink_fraction(rr->img);
        uint32_t best_page = 0;
        uint32_t last = rr->info.page_count < 16 ? rr->info.page_count : 16;  // bounded cost
        for (uint32_t p = 1; p < last && best_ink < 0.01; ++p) {
            RenderRequest r2 = rq;
            r2.page = (int)p;
            abt_image img = {};
            abt_file_info inf = rr->info;
            inf.flags &= ~(uint32_t)ABT_INFO_PAGE_EMPTY;
            bool np = false;
            if (FAILED(render_pdf(mf.data, mf.size, r2, fam, &img, &inf, &np))) break;  // e.g. deadline
            double ink = (inf.flags & ABT_INFO_PAGE_EMPTY) ? 0.0 : ink_fraction(img);
            if (ink > best_ink) {
                abt_image_free(&rr->img);
                rr->img = img;
                rr->info.flags &= ~(uint32_t)ABT_INFO_PAGE_EMPTY;
                best_ink = ink;
                best_page = p;
            } else {
                abt_image_free(&img);
            }
        }
        if (best_page)
            _snprintf_s(rr->reason, sizeof(rr->reason), _TRUNCATE, "first artboard with art (%u)", best_page + 1);
    }
    // A white tile tells the user nothing (D-016): when no page has visible art and there is no
    // usable document thumbnail (art only outside the artboards, saved by an Illustrator that wrote a
    // blank thumbnail), Explorer shows its icon instead. Remembered until the file changes.
    // Crop marks alone (well under 0.3 % ink) count as nothing for an Illustrator file.
    if (SUCCEEDED(hr) && canvas && (fam == ABT_FAM_AI_PDF || fam == ABT_FAM_PDF) &&
        ((rr->info.flags & ABT_INFO_PAGE_EMPTY) || (fam == ABT_FAM_AI_PDF && ink_fraction(rr->img) < 0.003))) {
        abt_image_free(&rr->img);
        rr->hr = HRESULT_FROM_WIN32(ERROR_NO_DATA);
        set_reason(rr, "no visible art on the artboards and no usable document thumbnail");
        return;
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

static DWORD g_seh_code;  // one render at a time per worker process

static int seh_filter(DWORD code) {
    g_seh_code = code;
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
        rr->hr = HRESULT_FROM_WIN32(mf.err ? mf.err : ERROR_OPEN_FAILED);
        set_reason(rr, "cannot open/map file");
    } else {
        if (!render_guarded(rq, mf, rr)) {
            abt_image_free(&rr->img);
            // A read error of the mapped view (network drop, file shortened while being saved) says
            // nothing about the file: ERROR_READ_FAULT is retried later instead of remembered.
            const bool io = g_seh_code == EXCEPTION_IN_PAGE_ERROR;
            rr->hr = HRESULT_FROM_WIN32(io ? ERROR_READ_FAULT : ERROR_FILE_CORRUPT);
            set_reason(rr, io ? "I/O error while reading the file" : "corrupt data while reading");
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
    if (st == ABT_E_NOTFOUND || st == ABT_E_TRUNCATED) {  /* no merged image, or a cut-off one */
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
