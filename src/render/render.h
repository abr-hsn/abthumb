// The render pipeline: probe -> embedded preview or full render -> BGRA at the requested size.
// Runs inside abthumb-render.exe (worker) and in abthumb.exe for tests/benchmarks.
// Never runs inside Explorer.
#pragma once
#include "abt_win.h"
#include "abt_image.h"
#include "abt_probe.h"

namespace abt {

struct RenderRequest {
    const wchar_t* path;
    int cx, cy;           // box; thumbnails use cx == cy
    int page;             // 0-based, or ABT_PAGE_CANVAS for the whole document (thumbnails)
    uint32_t mode;        // ABT_MODE_*
    uint32_t flags;       // ABT_RF_*
    uint32_t deadline_ms; // 0 = none
};

// Requested box with defaults and hard cap: cx falls back to 256, cy to cx, capped at 16384.
inline void request_box(const RenderRequest& rq, int* bw, int* bh) {
    int w = rq.cx > 0 ? rq.cx : 256;
    int h = rq.cy > 0 ? rq.cy : w;
    if (w > 16384) w = 16384;
    if (h > 16384) h = 16384;
    *bw = w;
    *bh = h;
}

struct RenderResult {
    HRESULT hr;
    abt_image img;        // BGRA premultiplied, fits in the box
    abt_file_info info;
    uint32_t source;      // ABT_SRC_*
    uint32_t elapsed_us;
    char reason[128];     // human-readable note (why a path was or was not taken)
    bool faulted;         // true if SEH caught an I/O fault (worker must exit after replying)
};

void render_shutdown();
void render_release_cached_doc();
bool render_has_cached_doc();
void render_file(const RenderRequest& rq, RenderResult* rr);
void render_result_free(RenderResult* rr);

// ---- internal, implemented in render_*.cpp
struct MappedFile {
    HANDLE file, map;
    const uint8_t* data;
    size_t size;
    size_t buf_cap;      // allocated capacity when !is_mapped
    DWORD err;           // Win32 error when map_file fails (0-byte files: ERROR_EMPTY)
    bool is_mapped;      // true = MapViewOfFile; false = ReadFile into buffer
    bool from_scratch;   // true = data eligible for reusable worker scratch buffer
    bool writable;       // true = copy-on-write view or private buffer (safe for in-place patching)
};
bool map_file(const wchar_t* path, MappedFile* mf);
void unmap_file(MappedFile* mf);
HRESULT fit_image(abt_image* img, int bw, int bh, bool allow_up);
void render_scratch_shutdown();
void gs_shutdown();

// Reusable PDFium document handle (single FPDF_LoadMemDocument64 per render_file).
struct PdfDoc;
void pdf_patch_ai_cmyk_icc(uint8_t* data, size_t size);
PdfDoc* pdf_open(const uint8_t* data, size_t size, abt_file_info* info, HRESULT* err_hr);
bool pdf_doc_layout(PdfDoc* doc, int* pages, float* w0, float* h0);
HRESULT pdf_doc_render(PdfDoc* doc, const RenderRequest& rq, abt_family fam, uint64_t deadline_qpc, abt_image* out,
                       abt_file_info* info, bool* no_pdf_content, double* out_ink);
void pdf_close(PdfDoc* doc);

// Convenience one-shot wrappers (open + use + close).
HRESULT render_pdf(const uint8_t* data, size_t size, const RenderRequest& rq, abt_family fam, abt_image* out,
                   abt_file_info* info, bool* no_pdf_content);
bool pdf_layout(const uint8_t* data, size_t size, int* pages, float* w0, float* h0);

// EPS / PostScript AI through an installed Ghostscript (D-004). S_FALSE-like E_NOTIMPL if absent.
HRESULT render_ghostscript(const wchar_t* path, const abt_probe& pr, const RenderRequest& rq, abt_image* out);

// PSD / PSB composite or layer fallback.
HRESULT render_psd(const uint8_t* data, size_t size, const abt_probe& pr, const RenderRequest& rq, abt_image* out,
                   char* reason, size_t reason_cap, bool* approx, bool abort_if_approx = false);

}  // namespace abt
