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
    int page;             // 0-based
    uint32_t mode;        // ABT_MODE_*
    uint32_t flags;       // ABT_RF_*
    uint32_t deadline_ms; // 0 = none
};

struct RenderResult {
    HRESULT hr;
    abt_image img;        // BGRA premultiplied, fits in the box
    abt_file_info info;
    uint32_t source;      // ABT_SRC_*
    uint32_t elapsed_us;
    char reason[128];     // human-readable note (why a path was or was not taken)
};

// One-time per process (PDFium init is lazy inside).
void render_init(bool background);
void render_shutdown();
void render_file(const RenderRequest& rq, RenderResult* rr);
void render_result_free(RenderResult* rr);

// ---- internal, implemented in render_*.cpp
struct MappedFile {
    HANDLE file, map;
    const uint8_t* data;
    size_t size;
};
bool map_file(const wchar_t* path, MappedFile* mf);
void unmap_file(MappedFile* mf);

// PDF / AI: render page into box. Fills info.page_count, pt sizes. Returns S_OK or failure;
// sets *no_pdf_content when an AI file only carries the "saved without PDF content" placeholder.
HRESULT render_pdf(const uint8_t* data, size_t size, const RenderRequest& rq, abt_family fam, abt_image* out,
                   abt_file_info* info, bool* no_pdf_content);

// EPS / PostScript AI through an installed Ghostscript (D-004). S_FALSE-like E_NOTIMPL if absent.
HRESULT render_ghostscript(const wchar_t* path, const abt_probe& pr, const RenderRequest& rq, abt_image* out);
bool ghostscript_available();

// PSD / PSB composite or layer fallback.
HRESULT render_psd(const uint8_t* data, size_t size, const abt_probe& pr, const RenderRequest& rq, abt_image* out,
                   char* reason, size_t reason_cap);

}  // namespace abt
