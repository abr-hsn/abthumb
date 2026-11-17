// PDF and PDF-based Illustrator rendering with PDFium (D-003). One thread per process.
#include "render.h"
#include "abt_proto.h"
#include <fpdfview.h>
#include <fpdf_progressive.h>
#include <fpdf_text.h>
#include <fpdf_edit.h>
#include <math.h>

namespace abt {

static bool g_pdfium_ready = false;

static void pdfium_init() {
    if (g_pdfium_ready) return;
    FPDF_LIBRARY_CONFIG cfg = {};
    cfg.version = 2;
    FPDF_InitLibraryWithConfig(&cfg);
    g_pdfium_ready = true;
}

void render_shutdown() {
    if (g_pdfium_ready) FPDF_DestroyLibrary();
    g_pdfium_ready = false;
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
// are checked, so normal artwork never pays for text extraction.
static bool is_ai_placeholder(FPDF_PAGE page) {
    int objs = FPDFPage_CountObjects(page);
    if (objs <= 0 || objs > 64) return false;
    FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
    if (!tp) return false;
    bool hit = false;
    int n = FPDFText_CountChars(tp);
    if (n > 0 && n < 4000) {
        unsigned short buf[4096];
        int got = FPDFText_GetText(tp, 0, n, buf);
        // ASCII-fold and search for "without PDF"
        char a[4096];
        int m = 0;
        for (int i = 0; i < got && m < 4095; ++i) a[m++] = (char)(buf[i] < 128 ? buf[i] : ' ');
        a[m] = 0;
        hit = strstr(a, "without PDF") != nullptr || strstr(a, "without PDF Content") != nullptr;
    }
    FPDFText_ClosePage(tp);
    return hit;
}

HRESULT render_pdf(const uint8_t* data, size_t size, const RenderRequest& rq, abt_family fam, abt_image* out,
                   abt_file_info* info, bool* no_pdf_content) {
    *no_pdf_content = false;
    pdfium_init();
    FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(data, size, nullptr);
    if (!doc) {
        unsigned long e = FPDF_GetLastError();
        if (e == FPDF_ERR_PASSWORD) info->flags |= ABT_INFO_ENCRYPTED;
        return HRESULT_FROM_WIN32(e == FPDF_ERR_PASSWORD ? ERROR_ACCESS_DENIED : ERROR_BAD_FORMAT);
    }
    int pages = FPDF_GetPageCount(doc);
    info->page_count = pages > 0 ? (uint32_t)pages : 0;
    int index = rq.page;
    if (index < 0 || index >= pages) index = 0;
    HRESULT hr = E_FAIL;
    FPDF_PAGE page = pages > 0 ? FPDF_LoadPage(doc, index) : nullptr;
    if (page) {
        float pw = FPDF_GetPageWidthF(page), ph = FPDF_GetPageHeightF(page);
        if (index == 0) { info->pt_width = pw; info->pt_height = ph; }
        if (fam == ABT_FAM_AI_PDF && index == 0 && is_ai_placeholder(page)) {
            *no_pdf_content = true;
        } else if (pw > 0 && ph > 0) {
            int bw = rq.cx > 0 ? rq.cx : 256, bh = rq.cy > 0 ? rq.cy : bw;
            double s = fmin(bw / (double)pw, bh / (double)ph);  // vector: enlarging is exact
            int dw = (int)floor(pw * s + 0.5), dh = (int)floor(ph * s + 0.5);
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            if (abt_image_alloc(out, dw, dh) == ABT_OK) {
                FPDF_BITMAP bmp = FPDFBitmap_CreateEx(dw, dh, FPDFBitmap_BGRA, out->px, dw * 4);
                if (bmp) {
                    // Artboards / pages are white in Illustrator and Acrobat.
                    FPDFBitmap_FillRect(bmp, 0, 0, dw, dh, 0xFFFFFFFF);
                    PauseCtx pc = {};
                    pc.base.version = 1;
                    pc.base.NeedToPauseNow = need_pause;
                    if (rq.deadline_ms) {
                        LARGE_INTEGER f;
                        QueryPerformanceFrequency(&f);
                        pc.deadline_qpc = qpc() + (uint64_t)f.QuadPart * rq.deadline_ms / 1000;
                    }
                    int flags = FPDF_ANNOT;
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
                if (FAILED(hr)) abt_image_free(out);
                else out->has_alpha = 0;  // filled white, opaque
            }
        }
        FPDF_ClosePage(page);
    }
    FPDF_CloseDocument(doc);
    return hr;
}

}  // namespace abt
