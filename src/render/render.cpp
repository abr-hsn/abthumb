#include "render.h"
#include "abt_proto.h"
#include "abt_decode.h"
#include "abt_psd.h"
#include <stdlib.h>

// Implemented in abt_eps.c: Photoshop EPS hex resource block -> JPEG bytes (malloc'd).

namespace abt {

// ------------------------------------------------------------------ reusable scratch buffers
// Avoids per-file CreateFileMapping/MapViewOfFile/UnmapViewOfFile/TLB shootdowns for files <= 4 MB,
// and avoids repeated malloc/free churn when base64-decoding XMP thumbnails.
static constexpr size_t kBufferedFileMax = 4u << 20;
static constexpr size_t kBase64MaxLen = 64u << 20;

static SRWLOCK g_file_buf_lock = SRWLOCK_INIT;
static uint8_t* g_file_buf = nullptr;
static size_t g_file_cap = 0;

static SRWLOCK g_b64_buf_lock = SRWLOCK_INIT;
static uint8_t* g_b64_buf = nullptr;
static size_t g_b64_cap = 0;

static uint8_t* take_buf(SRWLOCK* lock, uint8_t** slot, size_t* slot_cap, size_t need, size_t* out_cap) {
    uint8_t* p = nullptr;
    size_t cap = 0;
    AcquireSRWLockExclusive(lock);
    if (*slot) {
        p = *slot;
        cap = *slot_cap;
        *slot = nullptr;
        *slot_cap = 0;
    }
    ReleaseSRWLockExclusive(lock);
    if (cap < need) {
        free(p);
        size_t alloc = need < 65536 ? 65536 : need;
        p = (uint8_t*)malloc(alloc);
        cap = p ? alloc : 0;
    }
    *out_cap = cap;
    return p;
}

static void return_buf(SRWLOCK* lock, uint8_t** slot, size_t* slot_cap, uint8_t* p, size_t cap, size_t keep_max) {
    if (!p) return;
    if (cap > keep_max) { free(p); return; }
    AcquireSRWLockExclusive(lock);
    if (!*slot) {
        *slot = p;
        *slot_cap = cap;
        p = nullptr;
    }
    ReleaseSRWLockExclusive(lock);
    free(p);
}

void render_scratch_shutdown() {
    uint8_t* f = nullptr;
    AcquireSRWLockExclusive(&g_file_buf_lock);
    f = g_file_buf; g_file_buf = nullptr; g_file_cap = 0;
    ReleaseSRWLockExclusive(&g_file_buf_lock);
    free(f);

    uint8_t* b = nullptr;
    AcquireSRWLockExclusive(&g_b64_buf_lock);
    b = g_b64_buf; g_b64_buf = nullptr; g_b64_cap = 0;
    ReleaseSRWLockExclusive(&g_b64_buf_lock);
    free(b);
}

bool map_file(const wchar_t* path, MappedFile* mf) {
    memset(mf, 0, sizeof(*mf));
    // Never trigger a OneDrive / cloud-placeholder download on direct worker or CLI calls (D-033).
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) { mf->err = GetLastError(); return false; }
    if (attr & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN)) {
        mf->err = ERROR_CLOUD_FILE_NOT_IN_SYNC;
        return false;
    }
    // No FILE_FLAG_SEQUENTIAL_SCAN: PDF/AI/PSD reads are random-access (XMP head, trailer, RLE rows).
    mf->file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (mf->file == INVALID_HANDLE_VALUE) { mf->file = nullptr; mf->err = GetLastError(); return false; }
    LARGE_INTEGER sz;
    sz.QuadPart = -1;
    if (!GetFileSizeEx(mf->file, &sz)) {
        mf->err = GetLastError();
        CloseHandle(mf->file);
        mf->file = nullptr;
        return false;
    }
    if (sz.QuadPart <= 0 || (uint64_t)sz.QuadPart > (uint64_t)SIZE_MAX) {
        CloseHandle(mf->file);
        mf->file = nullptr;
        mf->err = sz.QuadPart == 0 ? ERROR_EMPTY : ERROR_FILE_TOO_LARGE;
        return false;
    }
    const size_t fsize = (size_t)sz.QuadPart;
    const bool slow = is_slow_path(path);

    // Small/medium local files (<= 4 MB) or tiny network files (<= 64 KB): single ReadFile into reusable
    // buffer (no mmap/TLB/SEH overhead). On SMB shares (> 64 KB), use MapViewOfFile so extracting a
    // 30 KB embedded XMP thumbnail from a 1.5 MB network file only transfers the touched 64 KB head.
    const size_t buf_limit = slow ? (64u << 10) : kBufferedFileMax;
    if (fsize <= buf_limit) {
        size_t cap = 0;
        uint8_t* buf = take_buf(&g_file_buf_lock, &g_file_buf, &g_file_cap, fsize, &cap);
        if (buf) {
            size_t total = 0;
            bool ok = true;
            while (total < fsize) {
                DWORD got = 0;
                if (!ReadFile(mf->file, buf + total, (DWORD)(fsize - total), &got, nullptr)) {
                    mf->err = GetLastError();
                    ok = false;
                    break;
                }
                if (!got) break;  // file shrank while reading
                total += got;
            }
            CloseHandle(mf->file);
            mf->file = nullptr;
            if (ok && total > 0) {
                mf->data = buf;
                mf->size = total;
                mf->buf_cap = cap;
                mf->is_mapped = false;
                mf->from_scratch = (cap <= kBufferedFileMax);
                mf->writable = true;
                return true;
            }
            return_buf(&g_file_buf_lock, &g_file_buf, &g_file_cap, buf, cap, kBufferedFileMax);
            if (!mf->err) mf->err = ERROR_READ_FAULT;
            return false;
        }
    }

    // Map with PAGE_WRITECOPY (FILE_MAP_COPY) on local drives so in-memory copy-on-write patches
    // (such as CMYK ICC fast-path) never touch the file on disk. Fall back to PAGE_READONLY if needed.
    bool copy_on_write = !slow;
    mf->map = CreateFileMappingW(mf->file, nullptr, copy_on_write ? PAGE_WRITECOPY : PAGE_READONLY, 0, 0, nullptr);
    if (!mf->map && copy_on_write) {
        copy_on_write = false;
        mf->map = CreateFileMappingW(mf->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    }
    if (!mf->map) { mf->err = GetLastError(); CloseHandle(mf->file); mf->file = nullptr; return false; }
    mf->data = (const uint8_t*)MapViewOfFile(mf->map, copy_on_write ? FILE_MAP_COPY : FILE_MAP_READ, 0, 0, 0);
    if (!mf->data) {
        DWORD e = GetLastError();
        CloseHandle(mf->map);
        CloseHandle(mf->file);
        memset(mf, 0, sizeof(*mf));
        mf->err = e;
        return false;
    }
    mf->size = fsize;
    mf->is_mapped = true;
    mf->writable = copy_on_write;
    return true;
}

void unmap_file(MappedFile* mf) {
    if (mf->is_mapped) {
        if (mf->data) UnmapViewOfFile(mf->data);
        if (mf->map) CloseHandle(mf->map);
        if (mf->file) CloseHandle(mf->file);
    } else if (mf->data) {
        uint8_t* p = const_cast<uint8_t*>(mf->data);
        if (mf->from_scratch)
            return_buf(&g_file_buf_lock, &g_file_buf, &g_file_cap, p, mf->buf_cap, kBufferedFileMax);
        else
            free(p);
        if (mf->file) CloseHandle(mf->file);
    }
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

HRESULT fit_image(abt_image* img, int bw, int bh, bool allow_up) {
    int dw, dh;
    fit_box(img->w, img->h, bw, bh, allow_up, &dw, &dh);
    if (dw == img->w && dh == img->h) return S_OK;
    abt_image r;
    if (abt_resize(img->px, img->w, img->h, (size_t)img->w * 4, dw, dh, &r) != ABT_OK) return E_OUTOFMEMORY;
    int was_opaque = !img->has_alpha;
    abt_image_free(img);
    *img = r;
    if (was_opaque) img->has_alpha = 0;
    else abt_image_scan_alpha(img);
    return S_OK;
}

static HRESULT decode_preview(const MappedFile& mf, const abt_preview& pv, int bw, int bh, abt_image* out) {
    if (pv.offset > mf.size || pv.length > mf.size - pv.offset || pv.length == 0) return E_FAIL;
    const uint8_t* p = mf.data + pv.offset;
    size_t len = (size_t)pv.length;
    int edge = bw > bh ? bw : bh;
    HRESULT hr = E_FAIL;
    bool already_fitted = false;
    switch (pv.kind) {
    case ABT_PV_JPEG:
    case ABT_PV_PNG:
    case ABT_PV_TIFF:
    case ABT_PV_JPEG_BGR: {
        const bool is_jpeg = pv.kind == ABT_PV_JPEG || pv.kind == ABT_PV_JPEG_BGR;
        already_fitted = true;
        if (pv.flags & ABT_PVF_BASE64) {
            if (len > kBase64MaxLen) return E_FAIL;
            size_t need = len / 4 * 3 + 4;
            size_t cap = 0;
            uint8_t* buf = take_buf(&g_b64_buf_lock, &g_b64_buf, &g_b64_cap, need, &cap);
            if (!buf) return E_OUTOFMEMORY;
            size_t n = abt_base64_decode(p, len, buf);
            hr = decode_image_bytes_box(buf, n, bw, bh, false, out, is_jpeg);
            return_buf(&g_b64_buf_lock, &g_b64_buf, &g_b64_cap, buf, cap, 4u << 20);
        } else if (pv.flags & ABT_PVF_HEX_PSRES) {
            uint8_t* jpeg = nullptr;
            size_t n = abt_eps_psres_jpeg(mf.data, mf.size, &pv, &jpeg);
            hr = n ? decode_image_bytes_box(jpeg, n, bw, bh, false, out, true) : E_FAIL;
            free(jpeg);
        } else {
            hr = decode_image_bytes_box(p, len, bw, bh, pv.kind == ABT_PV_JPEG_BGR, out, is_jpeg);
        }
        break;
    }
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
    if (SUCCEEDED(hr) && !already_fitted) hr = fit_image(out, bw, bh, false);
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

// Fast XMP layout check for AI files (avoids opening PDFium on the 256 px XMP fast path).
// Uses xmp_pages (<xmpTPg:NPages>) and xmp_unit_pts (<stDim:unit>Points) already parsed in abt_probe_pdf.
static bool ai_layout_from_xmp(const abt_probe& pr, int* pages, float* w0, float* h0) {
    if (pr.xmp_pages < 1 || pr.xmp_pages > 10000) return false;
    if (pr.xmp_pages > 1) {
        // Multi-artboard: ai_canvas_is_page1() is guaranteed false regardless of artboard-1 size.
        *pages = (int)pr.xmp_pages;
        *w0 = pr.info.pt_width;
        *h0 = pr.info.pt_height;
        return true;
    }
    // Single artboard: active artboard == artboard 1, safe when stDim is in Points.
    if (!pr.xmp_unit_pts || pr.info.pt_width <= 0 || pr.info.pt_height <= 0) return false;
    *pages = 1;
    *w0 = pr.info.pt_width;
    *h0 = pr.info.pt_height;
    return true;
}

// The whole-canvas thumbnail of a multi-artboard Illustrator file: Illustrator's own document
// thumbnail, enlarged when the request is bigger than it (soft, but it is the only image that holds
// art outside the artboards).
static int ai_best_xmp_preview(const abt_probe& pr, int edge) {
    int best_ge = -1, best_any = -1;
    uint32_t ge_len = 0xFFFFFFFFu, any_len = 0;
    for (int i = 0; i < pr.n_previews; ++i) {
        const abt_preview& pv = pr.previews[i];
        if (pv.source != ABT_PVS_XMP) continue;
        uint32_t l = pv.width > pv.height ? pv.width : pv.height;
        if (l >= (uint32_t)edge && l < ge_len) { ge_len = l; best_ge = i; }
        if (best_any < 0 || l > any_len) { any_len = l; best_any = i; }
    }
    return best_ge >= 0 ? best_ge : best_any;
}

static bool ai_canvas_thumbnail(const MappedFile& mf, const abt_probe& pr, int bw, int bh, RenderResult* rr) {
    int edge = bw > bh ? bw : bh;
    int best = ai_best_xmp_preview(pr, edge);
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

// High-resolution whole-artwork render for multi-artboard Illustrator files (edge > 256):
// composites crisp PDFium vector/image renders of each artboard onto the upscaled XMP document
// thumbnail (so off-artboard dimension labels and artwork are preserved while every artboard is
// crystal clear).
static bool ai_render_whole_canvas(const RenderRequest& rq, const MappedFile& mf, const abt_probe& pr,
                                   int bw, int bh, uint64_t deadline_qpc, PdfDoc* doc, RenderResult* rr) {
    if (!doc) return false;
    int best = ai_best_xmp_preview(pr, bw > bh ? bw : bh);
    if (best < 0) return false;
    float canvas_pt_w = pr.info.canvas_pt_w;
    float canvas_pt_h = pr.info.canvas_pt_h;
    if (!(canvas_pt_w > 0.0f && canvas_pt_h > 0.0f)) return false;

    abt_image thumb = {};
    if (FAILED(decode_preview(mf, pr.previews[best], 4096, 4096, &thumb))) return false;
    abt_image canvas_img = {};
    int cw = 0, ch = 0;
    fit_box(thumb.w, thumb.h, bw, bh, true, &cw, &ch);
    if (abt_resize(thumb.px, thumb.w, thumb.h, (size_t)thumb.w * 4u, cw, ch, &canvas_img) != ABT_OK) {
        abt_image_free(&thumb);
        return false;
    }
    canvas_img.has_alpha = 0;

    enum { kMaxCanvasPages = 32 };
    int npages = (int)rr->info.page_count;
    if (npages < 1) {
        int lp = 0; float w0 = 0, h0 = 0;
        if (pdf_doc_layout(doc, &lp, &w0, &h0)) npages = lp;
    }
    if (npages > kMaxCanvasPages) npages = kMaxCanvasPages;

    float pw[kMaxCanvasPages] = {};
    float ph[kMaxCanvasPages] = {};
    int order[kMaxCanvasPages] = {};
    int valid_pages = 0;
    for (int p = 0; p < npages; ++p) {
        if (pdf_doc_page_size(doc, p, &pw[p], &ph[p])) {
            order[valid_pages++] = p;
        }
    }
    // Place larger artboards first so smaller strips cannot match sub-regions of larger artboards.
    for (int i = 1; i < valid_pages; ++i) {
        int cur = order[i];
        float cur_a = pw[cur] * ph[cur];
        int j = i - 1;
        while (j >= 0 && pw[order[j]] * ph[order[j]] < cur_a) {
            order[j + 1] = order[j];
            --j;
        }
        order[j + 1] = cur;
    }

    abt_canvas_rect placed[kMaxCanvasPages] = {};
    int n_placed = 0;
    bool no_pdf = false;
    for (int i = 0; i < valid_pages; ++i) {
        if (deadline_qpc && qpc() > deadline_qpc) break;
        int p = order[i];
        int ab_w = (int)((double)cw * (double)pw[p] / (double)canvas_pt_w + 0.5);
        int ab_h = (int)((double)ch * (double)ph[p] / (double)canvas_pt_h + 0.5);
        int ab_edge = ab_w > ab_h ? ab_w : ab_h;
        if (ab_edge < 8) continue;

        RenderRequest prq = rq;
        prq.page = p;
        prq.cx = ab_w;
        prq.cy = ab_h;
        abt_image page_img = {};
        abt_file_info pinfo = rr->info;
        pinfo.flags &= ~(uint32_t)ABT_INFO_PAGE_EMPTY;
        bool page_no_pdf = false;
        HRESULT hr = pdf_doc_render(doc, prq, ABT_FAM_AI_PDF, deadline_qpc, &page_img, &pinfo, &page_no_pdf, nullptr);
        if (page_no_pdf) { no_pdf = true; abt_image_free(&page_img); break; }
        if (SUCCEEDED(hr) && !(pinfo.flags & ABT_INFO_PAGE_EMPTY) && page_img.px) {
            abt_canvas_place_artboard(&canvas_img, &thumb, canvas_pt_w, canvas_pt_h, &page_img, pw[p], ph[p],
                                      placed, &n_placed, kMaxCanvasPages);
        }
        abt_image_free(&page_img);
    }
    abt_image_free(&thumb);

    if (no_pdf || n_placed == 0) {
        abt_image_free(&canvas_img);
        return false;
    }
    rr->img = canvas_img;
    rr->hr = S_OK;
    rr->source = ABT_SRC_RENDERED;
    rr->info.flags |= ABT_INFO_HAS_PDF_CONTENT;
    set_reason(rr, "Illustrator whole canvas (high-res artboards)");
    return true;
}

// ------------------------------------------------------------------ persistent 1-document worker cache
struct CachedDoc {
    wchar_t path[ABT_MAX_PATH_CHARS + 1];
    uint64_t file_size;
    uint64_t mtime;
    MappedFile mf;
    abt_probe pr;
    abt_status probe_st;
    PdfDoc* pdf;
    HRESULT pdf_open_hr;
    bool cmyk_patched;
    bool max_quality;
};

static SRWLOCK g_doc_lock = SRWLOCK_INIT;
static CachedDoc* g_cached_doc = nullptr;

static void cached_doc_close(CachedDoc* cd) {
    if (!cd) return;
    pdf_close(cd->pdf);
    unmap_file(&cd->mf);
    free(cd);
}

void render_release_cached_doc() {
    CachedDoc* cd = nullptr;
    AcquireSRWLockExclusive(&g_doc_lock);
    cd = g_cached_doc;
    g_cached_doc = nullptr;
    ReleaseSRWLockExclusive(&g_doc_lock);
    cached_doc_close(cd);
}

bool render_has_cached_doc() {
    AcquireSRWLockShared(&g_doc_lock);
    bool has = g_cached_doc != nullptr;
    ReleaseSRWLockShared(&g_doc_lock);
    return has;
}

static uint64_t make_deadline_qpc(uint32_t deadline_ms) {
    if (!deadline_ms) return 0;
    static volatile LONG64 g_freq = 0;
    LONG64 freq = InterlockedCompareExchange64(&g_freq, 0, 0);
    if (!freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart > 0 ? f.QuadPart : 1;
        InterlockedExchange64(&g_freq, freq);
    }
    return qpc() + (uint64_t)freq * deadline_ms / 1000;
}

// Body of the pipeline; may fault on I/O errors of the mapped view (guarded by the caller).
static void render_inner(const RenderRequest& rq, CachedDoc* cd, RenderResult* rr) {
    const MappedFile& mf = cd->mf;
    const abt_probe& pr = cd->pr;
    abt_status st = cd->probe_st;
    rr->info = pr.info;
    if (st != ABT_OK) {
        rr->hr = HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
        _snprintf_s(rr->reason, sizeof(rr->reason), _TRUNCATE, "probe: %s", abt_status_str(st));
        return;
    }
    abt_family fam = (abt_family)pr.info.family;
    const bool canvas = (uint32_t)rq.page == ABT_PAGE_CANVAS;
    const int page = canvas ? 0 : rq.page;
    const uint64_t deadline_qpc = make_deadline_qpc(rq.deadline_ms);

    // Persistent PDFium document handle kept open across layout, page renders, and consecutive requests.
    auto ensure_pdf = [&]() -> PdfDoc* {
        if (!cd->pdf && cd->pdf_open_hr == S_OK) {
            if (fam == ABT_FAM_AI_PDF && cd->mf.writable && !cd->cmyk_patched && !(rq.flags & ABT_RF_MAX_QUALITY)) {
                pdf_patch_ai_cmyk_icc(const_cast<uint8_t*>(cd->mf.data), cd->mf.size);
                cd->cmyk_patched = true;
            }
            cd->pdf = pdf_open(mf.data, mf.size, &rr->info, &cd->pdf_open_hr);
        }
        return cd->pdf;
    };

    // Illustrator: artboard count and whether the whole canvas is more than artboard 1 (D-029).
    // Skip PDFium completely on ABT_MODE_FAST (network fast-preview path, D-033) and whenever
    // XMP metadata already determines layout for a canvas thumbnail.
    bool ai_multi = false;
    if (fam == ABT_FAM_AI_PDF && rq.mode != ABT_MODE_FAST) {
        int pages = 0;
        float w0 = 0, h0 = 0;
        bool have_layout = false;
        if (rq.mode != ABT_MODE_PROBE)
            have_layout = ai_layout_from_xmp(pr, &pages, &w0, &h0);
        if (!have_layout && ensure_pdf())
            have_layout = pdf_doc_layout(cd->pdf, &pages, &w0, &h0);
        if (have_layout) {
            rr->info.page_count = (uint32_t)pages;
            if (w0 > 0 && h0 > 0) {
                rr->info.pt_width = w0;  // stDim in the XMP is the active artboard, not artboard 1
                rr->info.pt_height = h0;
            }
            ai_multi = !ai_canvas_is_page1(pr, pages, w0, h0);
            if (ai_multi) rr->info.flags |= ABT_INFO_CANVAS_BEYOND_PAGE1;
        }
    } else if (fam == ABT_FAM_AI_PDF && rq.mode == ABT_MODE_FAST) {
        int pages = 0;
        float w0 = 0, h0 = 0;
        if (ai_layout_from_xmp(pr, &pages, &w0, &h0)) {
            rr->info.page_count = (uint32_t)pages;
            if (w0 > 0 && h0 > 0) {
                rr->info.pt_width = w0;
                rr->info.pt_height = h0;
            }
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
    int bw, bh;
    request_box(rq, &bw, &bh);
    bool allow_embedded = !(rq.flags & ABT_RF_NO_EMBEDDED) && rq.mode != ABT_MODE_RENDER && page == 0;
    int edge = bw > bh ? bw : bh;
    int best = abt_probe_best_preview(&pr, edge);

    if (ai_multi) {
        if (canvas) {
            // For high-res previews (edge > 256, e.g. Preview Pane Whole artwork view), render each
            // artboard via PDFium and composite onto the whole canvas so text and artwork are crisp.
            if (edge > 256 && rq.mode != ABT_MODE_FAST &&
                ai_render_whole_canvas(rq, mf, pr, bw, bh, deadline_qpc, ensure_pdf(), rr)) {
                return;
            }
            if (allow_embedded && ai_canvas_thumbnail(mf, pr, bw, bh, rr)) return;
        }
        // One artboard: that thumbnail shows more than the artboard, so it never stands in for it.
        allow_embedded = false;
        if (canvas) set_reason(rr, "no document thumbnail: artboard 1 only");
    }

    // 1) An embedded preview that is big enough is exact and cheapest (D-011).
    bool best_failed = false;
    if (allow_embedded && best >= 0 && preview_adequate(pr.previews[best], bw, bh)) {
        if (SUCCEEDED(decode_preview(mf, pr.previews[best], bw, bh, &rr->img))) {
            rr->hr = S_OK;
            rr->source = ABT_SRC_EMBEDDED;
            set_reason(rr, "embedded preview");
            return;
        }
        best_failed = true;
    }
    if (rq.mode == ABT_MODE_FAST) {
        if (allow_embedded && best >= 0 && !best_failed &&
            SUCCEEDED(decode_preview(mf, pr.previews[best], bw, bh, &rr->img))) {
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
    double page_ink = 1.0;
    switch (fam) {
    case ABT_FAM_AI_PDF:
    case ABT_FAM_PDF: {
        bool no_pdf = false;
        if (!ensure_pdf()) {
            hr = cd->pdf_open_hr;
        } else {
            const bool need_ink = canvas;
            hr = pdf_doc_render(cd->pdf, rq, fam, deadline_qpc, &rr->img, &rr->info, &no_pdf,
                                need_ink ? &page_ink : nullptr);
        }
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
        bool thumb_covers_approx = false;
        if (allow_embedded && best >= 0 && !best_failed) {
            const abt_preview& pv = pr.previews[best];
            uint32_t pl = pv.width > pv.height ? pv.width : pv.height;
            thumb_covers_approx = (pl != 0 && (uint32_t)edge <= pl * 2);
        }
        hr = render_psd(mf.data, mf.size, pr, rq, &rr->img, rr->reason, sizeof(rr->reason), &approx,
                        thumb_covers_approx);
        // Our layer composite skips adjustment layers and effects, so its colours can be off.
        // Photoshop's own thumbnail (resource 1036, ~160 px) has the right colours: prefer it for
        // thumbnails it can serve with at most 2x enlargement (D-022).
        if (SUCCEEDED(hr) && approx && thumb_covers_approx) {
            const abt_preview& pv = pr.previews[best];
            abt_image em = {};
            if (SUCCEEDED(decode_preview(mf, pv, bw, bh, &em))) {
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
            // Thumbnail decode failed after early abort: fall back to full layer composite.
            if (!rr->img.px)
                hr = render_psd(mf.data, mf.size, pr, rq, &rr->img, rr->reason, sizeof(rr->reason), &approx, false);
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
    bool fallback_timeout = false;
    if (SUCCEEDED(hr) && canvas && cd->pdf && (fam == ABT_FAM_AI_PDF || fam == ABT_FAM_PDF) &&
        rr->info.page_count > 1 &&
        ((rr->info.flags & ABT_INFO_PAGE_EMPTY) || (fam == ABT_FAM_AI_PDF && page_ink < 0.01))) {
        double best_ink = (rr->info.flags & ABT_INFO_PAGE_EMPTY) ? 0.0 : page_ink;
        uint32_t best_page = 0;
        uint32_t last = rr->info.page_count < 16 ? rr->info.page_count : 16;  // bounded cost
        for (uint32_t p = 1; p < last && best_ink < 0.01; ++p) {
            if (deadline_qpc && qpc() > deadline_qpc) { fallback_timeout = true; break; }
            RenderRequest r2 = rq;
            r2.page = (int)p;
            abt_image img = {};
            abt_file_info inf = rr->info;
            inf.flags &= ~(uint32_t)ABT_INFO_PAGE_EMPTY;
            bool np = false;
            double ink = 0.0;
            HRESULT hr2 = pdf_doc_render(cd->pdf, r2, fam, deadline_qpc, &img, &inf, &np, &ink);
            if (FAILED(hr2)) {
                if (hr2 == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) fallback_timeout = true;
                break;
            }
            if (inf.flags & ABT_INFO_PAGE_EMPTY) ink = 0.0;
            if (ink > best_ink) {
                abt_image_free(&rr->img);
                rr->img = img;
                rr->info.flags &= ~(uint32_t)ABT_INFO_PAGE_EMPTY;
                best_ink = ink;
                page_ink = ink;
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
        ((rr->info.flags & ABT_INFO_PAGE_EMPTY) || (fam == ABT_FAM_AI_PDF && page_ink < 0.003))) {
        abt_image_free(&rr->img);
        if (fallback_timeout) {
            // Do not permanently negative-cache a file whose fallback artboard scan hit the deadline.
            rr->hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            set_reason(rr, "timed out scanning artboards for visible art");
        } else {
            rr->hr = HRESULT_FROM_WIN32(ERROR_NO_DATA);
            set_reason(rr, "no visible art on the artboards and no usable document thumbnail");
        }
        return;
    }
    if (SUCCEEDED(hr)) {
        rr->hr = S_OK;
        rr->source = ABT_SRC_RENDERED;
        if (!rr->reason[0]) set_reason(rr, "rendered");
        return;
    }

    // 3) Smaller embedded preview beats nothing.
    if (allow_embedded && best >= 0 && !best_failed &&
        SUCCEEDED(decode_preview(mf, pr.previews[best], bw, bh, &rr->img))) {
        rr->hr = S_OK;
        rr->source = ABT_SRC_EMBEDDED | ABT_SRC_SCALED;
        if (!rr->reason[0]) set_reason(rr, "render failed, used smaller embedded preview");
        return;
    }
    rr->hr = FAILED(hr) ? hr : E_FAIL;
    if (!rr->reason[0]) _snprintf_s(rr->reason, sizeof(rr->reason), _TRUNCATE, "render failed 0x%08lx", hr);
}

// Catch only mapped-file I/O faults (EXCEPTION_IN_PAGE_ERROR) so we can return transient
// ERROR_READ_FAULT (D-033). Access violations are NOT swallowed: the worker crashes cleanly and
// the agent's pool respawns a fresh process.
static int seh_filter(DWORD code) {
    return code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

static bool render_guarded(const RenderRequest& rq, CachedDoc* cd, RenderResult* rr) {
    __try {
        render_inner(rq, cd, rr);
        return true;
    } __except (seh_filter(GetExceptionCode())) {
        return false;
    }
}

static bool probe_guarded(CachedDoc* cd) {
    __try {
        cd->probe_st = abt_probe_file(cd->mf.data, cd->mf.size, &cd->pr);
        return true;
    } __except (seh_filter(GetExceptionCode())) {
        return false;
    }
}

void render_file(const RenderRequest& rq, RenderResult* rr) {
    memset(rr, 0, sizeof(*rr));
    rr->hr = E_FAIL;
    uint64_t t0 = qpc();
    if (!rq.path || !rq.path[0]) {
        rr->hr = E_INVALIDARG;
        set_reason(rr, "empty path");
        return;
    }

    const bool slow = is_slow_path(rq.path);
    const bool max_q = (rq.flags & ABT_RF_MAX_QUALITY) != 0;
    uint64_t fsize = 0, fmtime = 0;
    bool have_stat = false;
    if (!slow) {
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (GetFileAttributesExW(rq.path, GetFileExInfoStandard, &fad)) {
            if (fad.dwFileAttributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN)) {
                rr->hr = HRESULT_FROM_WIN32(ERROR_CLOUD_FILE_NOT_IN_SYNC);
                set_reason(rr, "cloud placeholder not in sync");
                rr->elapsed_us = (uint32_t)(qpc_ms(t0, qpc()) * 1000.0);
                return;
            }
            fsize = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            fmtime = ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
            have_stat = fsize > 0;
        }
    }

    CachedDoc* cd = nullptr;
    AcquireSRWLockExclusive(&g_doc_lock);
    cd = g_cached_doc;
    g_cached_doc = nullptr;
    ReleaseSRWLockExclusive(&g_doc_lock);

    bool hit = cd && have_stat && cd->mf.data && cd->file_size == fsize && cd->mtime == fmtime &&
               cd->max_quality == max_q && ieq(cd->path, rq.path);
    if (!hit) {
        cached_doc_close(cd);
        cd = (CachedDoc*)calloc(1, sizeof(CachedDoc));
        if (!cd) {
            rr->hr = E_OUTOFMEMORY;
            set_reason(rr, "out of memory");
            return;
        }
        if (!map_file(rq.path, &cd->mf)) {
            rr->hr = HRESULT_FROM_WIN32(cd->mf.err ? cd->mf.err : ERROR_OPEN_FAILED);
            set_reason(rr, "cannot open/map file");
            free(cd);
            rr->elapsed_us = (uint32_t)(qpc_ms(t0, qpc()) * 1000.0);
            return;
        }
        if (!probe_guarded(cd)) {
            rr->faulted = true;
            rr->hr = HRESULT_FROM_WIN32(ERROR_READ_FAULT);
            set_reason(rr, "I/O error while reading the file");
            cached_doc_close(cd);
            rr->elapsed_us = (uint32_t)(qpc_ms(t0, qpc()) * 1000.0);
            return;
        }
        lstrcpynW(cd->path, rq.path, (int)_countof(cd->path));
        cd->file_size = fsize;
        cd->mtime = fmtime;
        cd->pdf_open_hr = S_OK;
        cd->max_quality = max_q;
    }

    if (!render_guarded(rq, cd, rr)) {
        abt_image_free(&rr->img);
        // A read error of the mapped view (network drop, file shortened while being saved) says
        // nothing about the file: ERROR_READ_FAULT is retried later instead of remembered.
        // Signal the worker to exit after replying so leaked/unwound PDFium/WIC state is discarded.
        rr->faulted = true;
        rr->hr = HRESULT_FROM_WIN32(ERROR_READ_FAULT);
        set_reason(rr, "I/O error while reading the file");
    }

    // Keep local PDF/AI documents cached in the worker across consecutive artboard renders.
    bool keep = !rr->faulted && !slow && have_stat && SUCCEEDED(rr->hr) &&
                (cd->pr.info.family == ABT_FAM_AI_PDF || cd->pr.info.family == ABT_FAM_PDF);
    if (keep) {
        CachedDoc* old = nullptr;
        AcquireSRWLockExclusive(&g_doc_lock);
        if (!g_cached_doc) {
            g_cached_doc = cd;
            cd = nullptr;
        } else {
            old = g_cached_doc;
            g_cached_doc = cd;
            cd = nullptr;
        }
        ReleaseSRWLockExclusive(&g_doc_lock);
        cached_doc_close(old);
    } else {
        cached_doc_close(cd);
    }

    if (FAILED(rr->hr)) abt_image_free(&rr->img);
    rr->elapsed_us = (uint32_t)(qpc_ms(t0, qpc()) * 1000.0);
}

void render_result_free(RenderResult* rr) { abt_image_free(&rr->img); }

// ------------------------------------------------------------------ PSD

HRESULT render_psd(const uint8_t* data, size_t size, const abt_probe& pr, const RenderRequest& rq, abt_image* out,
                   char* reason, size_t reason_cap, bool* approx, bool abort_if_approx) {
    *approx = false;
    if (!pr.info.px_width || !pr.info.px_height) {
        lstrcpynA(reason, "psd: invalid dimensions", (int)reason_cap);
        return E_FAIL;
    }
    int bw, bh;
    request_box(rq, &bw, &bh);
    abt_psd_opts o = {};
    fit_box((int)pr.info.px_width, (int)pr.info.px_height, bw, bh, false, &o.dst_w, &o.dst_h);
    o.rows_per_out = (rq.flags & ABT_RF_MAX_QUALITY) ? 0 : 4;
    o.abort_if_approx = abort_if_approx ? 1 : 0;
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
    if (!pb.px) return S_OK;  // early abort_if_approx hit: caller decodes embedded thumbnail
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
