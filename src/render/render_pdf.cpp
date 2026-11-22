// PDF and PDF-based Illustrator rendering with PDFium (D-003). One thread per process.
#include "render.h"
#include "abt_proto.h"
#include "abt_decode.h"
#include "abt_wic.h"
#include <fpdfview.h>
#include <fpdf_progressive.h>
#include <fpdf_text.h>
#include <fpdf_edit.h>
#include <emmintrin.h>
#include <math.h>
#include <stdlib.h>

namespace abt {

static bool g_pdfium_ready = false;
static uint64_t g_qpc_freq_pdf = 0;

static void pdfium_init() {
    if (g_pdfium_ready) return;
    FPDF_LIBRARY_CONFIG cfg = {};
    cfg.version = 2;
    FPDF_InitLibraryWithConfig(&cfg);
    g_pdfium_ready = true;
}

static uint64_t qpc_deadline_from_ms(uint32_t ms) {
    if (!ms) return 0;
    if (!g_qpc_freq_pdf) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpc_freq_pdf = f.QuadPart > 0 ? (uint64_t)f.QuadPart : 1;
    }
    return qpc() + g_qpc_freq_pdf * ms / 1000;
}

void render_shutdown() {
    if (g_pdfium_ready) FPDF_DestroyLibrary();
    g_pdfium_ready = false;
    cmyk_xform_shutdown();
    wic_shutdown();
    gs_shutdown();
    render_scratch_shutdown();
}

struct PauseCtx {
    IFSDK_PAUSE base;
    uint64_t deadline_qpc;  // 0 = none
};

static FPDF_BOOL need_pause(IFSDK_PAUSE* p) {
    PauseCtx* c = (PauseCtx*)p;
    return c->deadline_qpc && qpc() > c->deadline_qpc;
}

// Illustrator's placeholder page for files saved without PDF content contains a short text
// ("This is an Adobe Illustrator File that was saved without PDF Content..."). Only small pages
// with at least one text object are checked, so pure vector artwork never pays for text extraction.
static bool is_ai_placeholder(FPDF_PAGE page) {
    int objs = FPDFPage_CountObjects(page);
    if (objs <= 0 || objs > 64) return false;
    bool has_text = false;
    for (int i = 0; i < objs; ++i) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
        int type = obj ? FPDFPageObj_GetType(obj) : FPDF_PAGEOBJ_UNKNOWN;
        if (type == FPDF_PAGEOBJ_TEXT || type == FPDF_PAGEOBJ_FORM) {
            has_text = true;
            break;
        }
    }
    if (!has_text) return false;
    FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
    if (!tp) return false;
    bool hit = false;
    int n = FPDFText_CountChars(tp);
    if (n > 0 && n < 4000) {
        unsigned short buf[4096];
        int got = FPDFText_GetText(tp, 0, n, buf);
        if (got > 0) {
            char a[4096];
            int lim = got < (int)(sizeof(a) - 1) ? got : (int)(sizeof(a) - 1);
            for (int i = 0; i < lim; ++i) a[i] = (char)((buf[i] > 0 && buf[i] < 128) ? buf[i] : ' ');
            a[lim] = 0;
            hit = strstr(a, "without PDF") != nullptr;
        }
    }
    FPDFText_ClosePage(tp);
    return hit;
}

// Fused SSE2 scan for empty page (all pixels == 0xFFFFFFFF) and ink fraction (any B,G,R < 230).
// When out_ink == nullptr, exits on the first non-white pixel. When out_ink != nullptr, exits early
// once ink >= 1% (0.01), which suffices for both the 1% multi-artboard threshold and 0.3% blank cap.
static bool scan_page_ink(const uint8_t* px, size_t n, double* out_ink) {
    if (!px || n == 0) {
        if (out_ink) *out_ink = 0.0;
        return true;
    }
    static const uint8_t kPop4[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
    const __m128i all_ff = _mm_set1_epi32(-1);
    const __m128i alpha_ff = _mm_set1_epi32((int)0xFF000000u);
    const __m128i thresh = _mm_set1_epi8(-26);  // 230u as signed int8
    const size_t ink_stop = n / 100 + 1;  // 1% threshold for early exit

    bool any_nonwhite = false;
    size_t ink = 0;
    size_t i = 0;

    if (!out_ink) {
        for (; i + 4 <= n; i += 4) {
            __m128i v = _mm_loadu_si128((const __m128i*)(px + i * 4));
            if (_mm_movemask_epi8(_mm_cmpeq_epi32(v, all_ff)) != 0xFFFF) return false;
        }
        const uint32_t* p32 = (const uint32_t*)px;
        for (; i < n; ++i)
            if (p32[i] != 0xFFFFFFFFu) return false;
        return true;
    }

    for (; i + 4 <= n; i += 4) {
        __m128i v = _mm_loadu_si128((const __m128i*)(px + i * 4));
        if (_mm_movemask_epi8(_mm_cmpeq_epi32(v, all_ff)) != 0xFFFF) {
            any_nonwhite = true;
            __m128i vm = _mm_or_si128(v, alpha_ff);
            __m128i ge = _mm_cmpeq_epi8(_mm_max_epu8(vm, thresh), vm);
            __m128i no_ink = _mm_cmpeq_epi32(ge, all_ff);
            int mask = (~_mm_movemask_ps(_mm_castsi128_ps(no_ink))) & 0xF;
            ink += kPop4[mask];
            if (ink >= ink_stop) {
                *out_ink = (double)ink / (double)n;
                return false;
            }
        }
    }
    const uint8_t* p = px + i * 4;
    for (; i < n; ++i, p += 4) {
        if (p[0] != 255 || p[1] != 255 || p[2] != 255 || p[3] != 255) {
            any_nonwhite = true;
            if (p[0] < 230 || p[1] < 230 || p[2] < 230) ++ink;
        }
    }
    *out_ink = (double)ink / (double)n;
    return !any_nonwhite;
}

PdfDoc* pdf_open(const uint8_t* data, size_t size, abt_file_info* info, HRESULT* err_hr) {
    pdfium_init();
    FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(data, size, nullptr);
    if (!doc) {
        unsigned long e = FPDF_GetLastError();
        if (e == FPDF_ERR_PASSWORD && info) info->flags |= ABT_INFO_ENCRYPTED;
        if (err_hr) *err_hr = HRESULT_FROM_WIN32(e == FPDF_ERR_PASSWORD ? ERROR_ACCESS_DENIED : ERROR_BAD_FORMAT);
        return nullptr;
    }
    if (err_hr) *err_hr = S_OK;
    return (PdfDoc*)doc;
}

void pdf_close(PdfDoc* doc) {
    if (doc) FPDF_CloseDocument((FPDF_DOCUMENT)doc);
}

bool pdf_doc_layout(PdfDoc* doc, int* pages, float* w0, float* h0) {
    *pages = 0;
    *w0 = *h0 = 0;
    if (!doc) return false;
    FPDF_DOCUMENT d = (FPDF_DOCUMENT)doc;
    int n = FPDF_GetPageCount(d);
    FS_SIZEF sz = {};
    bool ok = n > 0 && FPDF_GetPageSizeByIndexF(d, 0, &sz) && isfinite(sz.width) && isfinite(sz.height) &&
              sz.width > 0 && sz.height > 0;
    if (ok) {
        *pages = n;
        *w0 = sz.width;
        *h0 = sz.height;
    }
    return ok;
}

HRESULT pdf_doc_render(PdfDoc* doc, const RenderRequest& rq, abt_family fam, uint64_t deadline_qpc, abt_image* out,
                       abt_file_info* info, bool* no_pdf_content, double* out_ink) {
    *no_pdf_content = false;
    if (out_ink) *out_ink = 0.0;
    if (!doc || !out || !info) return E_INVALIDARG;
    out->px = nullptr;
    out->w = out->h = out->has_alpha = 0;
    FPDF_DOCUMENT d = (FPDF_DOCUMENT)doc;
    int pages = FPDF_GetPageCount(d);
    info->page_count = pages > 0 ? (uint32_t)pages : 0;
    int index = (uint32_t)rq.page == ABT_PAGE_CANVAS ? 0 : rq.page;
    if (index < 0 || index >= pages) {
        // A page that does not exist (the file changed, or a stale request) is an error, not page 1:
        // the preview pane would otherwise show artboard 1 labelled as another artboard.
        return HRESULT_FROM_WIN32(ERROR_INVALID_INDEX);
    }
    HRESULT hr = E_FAIL;
    FPDF_PAGE page = FPDF_LoadPage(d, index);
    if (page) {
        float pw = FPDF_GetPageWidthF(page), ph = FPDF_GetPageHeightF(page);
        info->pt_width = pw;  // size of the page / artboard shown
        info->pt_height = ph;
        if (fam == ABT_FAM_AI_PDF && index == 0 && is_ai_placeholder(page)) {
            *no_pdf_content = true;
        } else if (isfinite(pw) && isfinite(ph) && pw > 0 && ph > 0) {
            int bw, bh;
            request_box(rq, &bw, &bh);
            double s = fmin(bw / (double)pw, bh / (double)ph);  // vector: enlarging is exact
            int dw = (int)floor(pw * s + 0.5), dh = (int)floor(ph * s + 0.5);
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            if (dw <= ABT_MAX_DIM && dh <= ABT_MAX_DIM &&
                (uint64_t)dw * (uint64_t)dh * 4u <= ABT_MAX_PIXEL_BYTES) {
                // Allocate uninitialized and white-fill once (avoids calloc zero-fill + FillRect double write).
                const size_t nbytes = (size_t)dw * (size_t)dh * 4;
                uint8_t* px = (uint8_t*)malloc(nbytes);
                if (px) {
                    memset(px, 0xFF, nbytes);
                    out->px = px;
                    out->w = dw;
                    out->h = dh;
                    out->has_alpha = 0;
                    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(dw, dh, FPDFBitmap_BGRA, out->px, dw * 4);
                    if (bmp) {
                        PauseCtx pc = {};
                        pc.base.version = 1;
                        pc.base.NeedToPauseNow = need_pause;
                        pc.deadline_qpc = deadline_qpc ? deadline_qpc : qpc_deadline_from_ms(rq.deadline_ms);
                        // Illustrator files have no interactive PDF annotations; skip annotation traversal.
                        int flags = (fam == ABT_FAM_AI_PDF) ? 0 : FPDF_ANNOT;
                        int st = FPDF_RenderPageBitmap_Start(bmp, page, 0, 0, dw, dh, 0, flags, &pc.base);
                        while (st == FPDF_RENDER_TOBECONTINUED) {
                            if (pc.deadline_qpc && qpc() > pc.deadline_qpc) break;
                            st = FPDF_RenderPage_Continue(page, &pc.base);
                        }
                        FPDF_RenderPage_Close(page);
                        if (st == FPDF_RENDER_DONE) hr = S_OK;
                        else if (st == FPDF_RENDER_TOBECONTINUED) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                        FPDFBitmap_Destroy(bmp);
                    }
                    if (FAILED(hr)) {
                        abt_image_free(out);
                    } else {
                        out->has_alpha = 0;  // filled white, opaque
                        // Nothing visible on the page (no objects, or only white / invisible ones): the
                        // preview pane skips such artboards instead of showing a blank one (D-031).
                        bool empty = FPDFPage_CountObjects(page) == 0;
                        if (!empty) empty = scan_page_ink(out->px, (size_t)dw * (size_t)dh, out_ink);
                        if (empty) info->flags |= ABT_INFO_PAGE_EMPTY;
                    }
                } else {
                    hr = E_OUTOFMEMORY;
                }
            }
        }
        FPDF_ClosePage(page);
    }
    return hr;
}

bool pdf_layout(const uint8_t* data, size_t size, int* pages, float* w0, float* h0) {
    PdfDoc* doc = pdf_open(data, size, nullptr, nullptr);
    if (!doc) {
        *pages = 0;
        *w0 = *h0 = 0;
        return false;
    }
    bool ok = pdf_doc_layout(doc, pages, w0, h0);
    pdf_close(doc);
    return ok;
}

HRESULT render_pdf(const uint8_t* data, size_t size, const RenderRequest& rq, abt_family fam, abt_image* out,
                   abt_file_info* info, bool* no_pdf_content) {
    HRESULT err = E_FAIL;
    PdfDoc* doc = pdf_open(data, size, info, &err);
    if (!doc) {
        *no_pdf_content = false;
        return err;
    }
    HRESULT hr = pdf_doc_render(doc, rq, fam, 0, out, info, no_pdf_content, nullptr);
    pdf_close(doc);
    return hr;
}

}  // namespace abt
