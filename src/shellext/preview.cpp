// IPreviewHandler: one page / artboard at a time, rendered by the agent at device pixels (D-030).
// * First paint of a file comes from the thumbnail tier (usually cached, sub-millisecond) when that
//   image is the same picture as page 1; Illustrator files whose canvas is more than artboard 1 skip it.
// * Navigation: arrow / Page keys, Home / End, mouse wheel, hover arrows on the image, and the
//   buttons in the info strip. Pages already shown are kept as bitmaps here (instant flips back),
//   and the agent pre-renders the neighbours of the page on screen (prefetch), so stepping
//   through artboards rarely waits.
// * Requests run on a background thread; the newest request always wins.
#include "shellext.h"
#include "abt_wic.h"
#include "abt_proto.h"
#include <shobjidl.h>
#include <shlwapi.h>
#include <ocidl.h>
#include <windowsx.h>
#include <math.h>

namespace {

const wchar_t kWndClass[] = L"abthumbPreviewWnd";
const UINT WM_APP_RESULT = WM_APP + 1;
const UINT_PTR kResizeTimer = 1;
const int kPageCache = 5;  // bitmaps of pages already rendered for the current file

// Shared between the handler (UI thread) and its request thread; refcounted so the thread can
// finish a slow request after the handler is gone.
struct Shared {
    volatile LONG refs;
    SRWLOCK lock;
    HANDLE wake;
    bool stop;
    // pending request
    bool has_req;
    bool want_pixels;  // false: only prefetch the neighbours of `page` (it is shown from our cache)
    int gen, file_gen, cx, cy, page, count, dir;
    wchar_t* path;
    HWND notify;
};

struct Result {
    int gen, file_gen, page;
    int req_w, req_h;
    bool quick;  // low-res first paint from the thumbnail tier
    HRESULT hr;
    int w, h;
    uint8_t* px;  // premultiplied BGRA
    abt_file_info info;
    char reason[100];
};

void free_result(Result* r) {
    if (!r) return;
    abt::mem_free(r->px);
    abt::mem_free(r);
}

void shared_release(Shared* s) {
    if (InterlockedDecrement(&s->refs) == 0) {
        CloseHandle(s->wake);
        abt::mem_free(s->path);
        abt::mem_free(s);
    }
}

void post_result(Shared* s, Result* r) {
    HWND w;
    AcquireSRWLockShared(&s->lock);
    w = s->notify;
    ReleaseSRWLockShared(&s->lock);
    if (!w || !PostMessageW(w, WM_APP_RESULT, 0, (LPARAM)r)) free_result(r);
}

bool new_request_pending(Shared* s) {
    AcquireSRWLockShared(&s->lock);
    bool p = s->has_req || s->stop;
    ReleaseSRWLockShared(&s->lock);
    return p;
}

Result* fetch(uint16_t msg, const wchar_t* path, int cx, int cy, int page, int gen, int file_gen, bool quick) {
    Result* r = (Result*)abt::mem_zalloc(sizeof(Result));
    if (!r) return nullptr;
    abt_resp resp;
    r->gen = gen;
    r->file_gen = file_gen;
    r->page = page;
    r->req_w = cx;
    r->req_h = cy;
    r->quick = quick;
    r->hr = request_image(msg, path, (UINT)cx, (UINT)cy, (UINT)page, &resp, &r->px);
    r->w = (int)resp.width;
    r->h = (int)resp.height;
    r->info = resp.info;
    memcpy(r->reason, resp.reason, sizeof(r->reason));
    r->reason[sizeof(r->reason) - 1] = 0;
    if (FAILED(r->hr) || !r->px || r->w <= 0 || r->h <= 0) {
        abt::mem_free(r->px);
        r->px = nullptr;
        if (SUCCEEDED(r->hr)) r->hr = E_FAIL;
    }
    return r;
}

// Asks the agent to render the pages next to `page` (in the direction of travel first). Each call
// returns at once; the renders land in the agent's cache, and a later request for one of those pages
// joins the render if it is still running. Stops as soon as the user asks for something else.
void prefetch_neighbours(Shared* s, const wchar_t* path, int cx, int cy, int page, int count, int dir) {
    int order[2] = {page + (dir < 0 ? -1 : 1), page + (dir < 0 ? 1 : -1)};
    for (int i = 0; i < 2; ++i) {
        int p = order[i];
        if (p < 0 || p >= count || new_request_pending(s)) continue;
        abt_resp resp;
        uint8_t* px = nullptr;
        request_image(ABT_MSG_PREVIEW, path, (UINT)cx, (UINT)cy, (UINT)p, &resp, &px, ABT_RF_PREFETCH);
        abt::mem_free(px);
    }
}

DWORD WINAPI request_thread(void* arg) {
    Shared* s = (Shared*)arg;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int quick_done_for = -1;  // file generation whose first paint was already sent
    for (;;) {
        WaitForSingleObject(s->wake, INFINITE);
        AcquireSRWLockExclusive(&s->lock);
        bool stop = s->stop, has = s->has_req, pixels = s->want_pixels;
        int gen = s->gen, file_gen = s->file_gen, cx = s->cx, cy = s->cy, page = s->page;
        int count = s->count, dir = s->dir;
        wchar_t* path = nullptr;
        if (has && s->path) {
            size_t n = (size_t)lstrlenW(s->path) + 1;
            path = (wchar_t*)abt::mem_alloc(n * sizeof(wchar_t));
            if (path) memcpy(path, s->path, n * sizeof(wchar_t));
        }
        s->has_req = false;
        ReleaseSRWLockExclusive(&s->lock);
        if (stop) { abt::mem_free(path); break; }
        if (!path) continue;
        if (pixels) {
            // First paint of a new file: the thumbnail tier is usually cached already (sub-ms).
            if (quick_done_for != file_gen && page == 0) {
                quick_done_for = file_gen;
                if (Result* q = fetch(ABT_MSG_THUMB, path, 256, 256, 0, gen, file_gen, true)) {
                    if (q->info.page_count > (uint32_t)count) count = (int)q->info.page_count;
                    post_result(s, q);
                }
            }
            if (Result* r = fetch(ABT_MSG_PREVIEW, path, cx, cy, page, gen, file_gen, false)) {
                if (r->info.page_count > (uint32_t)count) count = (int)r->info.page_count;
                post_result(s, r);
            }
        }
        if (count > 1) prefetch_neighbours(s, path, cx, cy, page, count, dir);
        abt::mem_free(path);
    }
    CoUninitialize();
    shared_release(s);
    // The thread can outlive every COM object, so DllCanUnloadNow may already say yes: drop the
    // module reference taken in DoPreview and exit without returning into unloaded code.
    FreeLibraryAndExitThread(g_dll, 0);
}

// ------------------------------------------------------------------ drawing helpers

COLORREF mix(COLORREF a, COLORREF b, int pct_b) {
    int r = (GetRValue(a) * (100 - pct_b) + GetRValue(b) * pct_b) / 100;
    int g = (GetGValue(a) * (100 - pct_b) + GetGValue(b) * pct_b) / 100;
    int bl = (GetBValue(a) * (100 - pct_b) + GetBValue(b) * pct_b) / 100;
    return RGB(r, g, bl);
}

bool is_dark(COLORREF c) { return GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114 < 128000; }

void fill(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void round_fill(HDC dc, const RECT& r, int radius, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, radius, radius);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
}

bool in_rect(const RECT& r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

struct PageBmp {
    HBITMAP bmp;
    int page, w, h, req_w, req_h;
    bool alpha;
    unsigned use;  // LRU stamp
};

enum Zone { Z_NONE = 0, Z_IMG_PREV, Z_IMG_NEXT, Z_BTN_PREV, Z_BTN_NEXT };

class PreviewHandler final : public IPreviewHandler,
                             public IPreviewHandlerVisuals,
                             public IObjectWithSite,
                             public IOleWindow,
                             public IInitializeWithItem,
                             public IInitializeWithFile {
    LONG m_ref = 1;
    HWND m_parent = nullptr;
    HWND m_wnd = nullptr;
    RECT m_rect = {};
    IUnknown* m_site = nullptr;
    wchar_t* m_path = nullptr;
    COLORREF m_bg = RGB(255, 255, 255);
    COLORREF m_fg = RGB(30, 30, 30);
    Shared* m_shared = nullptr;
    // pages of the current file
    PageBmp m_cache[kPageCache] = {};
    unsigned m_use = 0;
    // displayed state
    HBITMAP m_bmp = nullptr;  // owned by m_cache unless m_quick
    int m_bw = 0, m_bh = 0;
    bool m_alpha = false;
    bool m_quick = false;     // low-res first paint (owned here, not cached)
    bool m_loading = false;   // the page on screen is not the page asked for yet
    abt_file_info m_info = {};
    bool m_have_info = false;
    int m_page = 0, m_dir = 1;
    int m_gen = 0, m_file_gen = 0;
    int m_req_w = 0, m_req_h = 0;
    char m_error[100] = "";
    // input
    int m_zone = Z_NONE;
    bool m_tracking = false;
    int m_wheel = 0;
    // GDI objects, rebuilt when the DPI changes
    int m_font_dpi = 0;
    HFONT m_font = nullptr, m_icons = nullptr, m_icons_big = nullptr;
    HBRUSH m_checker = nullptr;
    COLORREF m_checker_bg = 0;

    int dpi() const { return m_wnd ? (int)GetDpiForWindow(m_wnd) : 96; }
    int px(int v) const { return MulDiv(v, dpi(), 96); }
    int strip_h() const { return px(30); }
    int page_count() const { return m_have_info && m_info.page_count > 1 ? (int)m_info.page_count : 1; }
    bool can_go(int delta) const { int p = m_page + delta; return p >= 0 && p < page_count(); }

    void image_area(RECT* r) const {
        GetClientRect(m_wnd, r);
        r->bottom -= strip_h();
        if (r->bottom < r->top) r->bottom = r->top;
    }
    // Navigation buttons at the right end of the info strip.
    void strip_buttons(RECT* prev, RECT* label, RECT* next) const {
        RECT rc;
        GetClientRect(m_wnd, &rc);
        int sh = strip_h(), b = px(26), lw = px(64), pad = px(4);
        int top = rc.bottom - sh + (sh - b) / 2;
        *next = {rc.right - pad - b, top, rc.right - pad, top + b};
        *label = {next->left - lw, top, next->left, top + b};
        *prev = {label->left - b, top, label->left, top + b};
    }
    // Hover arrows over the image: left / right bands of the image area.
    void image_arrows(RECT* prev, RECT* next) const {
        RECT a;
        image_area(&a);
        int d = px(40), m = px(10);
        int cy = (a.top + a.bottom) / 2;
        *prev = {a.left + m, cy - d / 2, a.left + m + d, cy + d / 2};
        *next = {a.right - m - d, cy - d / 2, a.right - m, cy + d / 2};
    }

    void ensure_gdi() {
        int d = dpi();
        if (d != m_font_dpi) {
            if (m_font) DeleteObject(m_font);
            if (m_icons) DeleteObject(m_icons);
            if (m_icons_big) DeleteObject(m_icons_big);
            m_font = CreateFontW(-MulDiv(12, d, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                 CLEARTYPE_QUALITY, 0, L"Segoe UI");
            // Segoe MDL2 Assets ships with Windows 10 and 11 (chevrons E76B / E76C).
            m_icons = CreateFontW(-MulDiv(11, d, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                  CLEARTYPE_QUALITY, 0, L"Segoe MDL2 Assets");
            m_icons_big = CreateFontW(-MulDiv(16, d, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                      CLEARTYPE_QUALITY, 0, L"Segoe MDL2 Assets");
            m_font_dpi = d;
            if (m_checker) { DeleteObject(m_checker); m_checker = nullptr; }
        }
        if (!m_checker || m_checker_bg != m_bg) {
            // Transparency checkerboard (cells of 8 px at 96 DPI), tinted to the pane's theme.
            if (m_checker) DeleteObject(m_checker);
            int c = px(8);
            COLORREF a = is_dark(m_bg) ? RGB(58, 58, 58) : RGB(255, 255, 255);
            COLORREF b = is_dark(m_bg) ? RGB(76, 76, 76) : RGB(226, 226, 226);
            void* bits = nullptr;
            HBITMAP pat = abt::create_dib32(c * 2, c * 2, &bits);
            m_checker = nullptr;
            if (pat) {
                uint32_t* p = (uint32_t*)bits;
                for (int y = 0; y < c * 2; ++y)
                    for (int x = 0; x < c * 2; ++x) {
                        COLORREF v = ((x / c) ^ (y / c)) & 1 ? b : a;
                        p[y * c * 2 + x] = 0xFF000000u | (GetRValue(v) << 16) | (GetGValue(v) << 8) | GetBValue(v);
                    }
                m_checker = CreatePatternBrush(pat);
                DeleteObject(pat);  // the brush keeps its own copy
            }
            m_checker_bg = m_bg;
        }
    }
    void free_gdi() {
        if (m_font) DeleteObject(m_font);
        if (m_icons) DeleteObject(m_icons);
        if (m_icons_big) DeleteObject(m_icons_big);
        if (m_checker) DeleteObject(m_checker);
        m_font = m_icons = m_icons_big = nullptr;
        m_checker = nullptr;
        m_font_dpi = 0;
    }

    // ---- page cache

    PageBmp* cache_find(int page) {
        PageBmp* best = nullptr;
        for (int i = 0; i < kPageCache; ++i) {
            PageBmp& c = m_cache[i];
            if (c.bmp && c.page == page && (!best || c.req_w * c.req_h > best->req_w * best->req_h)) best = &c;
        }
        return best;
    }
    void cache_clear() {
        if (m_bmp && m_quick) DeleteObject(m_bmp);
        m_bmp = nullptr;
        m_quick = false;
        for (int i = 0; i < kPageCache; ++i)
            if (m_cache[i].bmp) DeleteObject(m_cache[i].bmp);
        memset(m_cache, 0, sizeof(m_cache));
    }
    // Takes the pixels of a result into a cached bitmap; returns the slot (or null).
    PageBmp* cache_put(Result* r) {
        void* bits = nullptr;
        HBITMAP bmp = abt::create_dib32(r->w, r->h, &bits);
        if (!bmp) return nullptr;
        size_t n = (size_t)r->w * r->h;
        memcpy(bits, r->px, n * 4);  // premultiplied: ready for AlphaBlend
        bool alpha = false;
        for (size_t i = 0; i < n; ++i)
            if (r->px[i * 4 + 3] != 255) { alpha = true; break; }
        PageBmp* slot = nullptr;
        for (int i = 0; i < kPageCache; ++i) {  // replace the same page, else a free slot, else the LRU
            PageBmp& c = m_cache[i];
            if (c.bmp && c.page == r->page) { slot = &c; break; }
        }
        for (int i = 0; i < kPageCache && !slot; ++i)
            if (!m_cache[i].bmp) slot = &m_cache[i];
        if (!slot) {  // least recently used, but never the bitmap on screen
            for (int i = 0; i < kPageCache; ++i)
                if (m_cache[i].bmp != m_bmp && (!slot || m_cache[i].use < slot->use)) slot = &m_cache[i];
        }
        if (!slot) { DeleteObject(bmp); return nullptr; }
        bool on_screen = slot->bmp && slot->bmp == m_bmp;  // same page re-rendered (e.g. after a resize)
        if (slot->bmp) DeleteObject(slot->bmp);
        *slot = {bmp, r->page, r->w, r->h, r->req_w, r->req_h, alpha, ++m_use};
        if (on_screen) {
            m_bmp = nullptr;
            show(slot);
        }
        return slot;
    }
    void show(PageBmp* c) {
        if (m_bmp && m_quick) DeleteObject(m_bmp);
        m_quick = false;
        m_bmp = c->bmp;
        m_bw = c->w;
        m_bh = c->h;
        m_alpha = c->alpha;
        c->use = ++m_use;
    }
    // A cached page is good enough when it was rendered for (about) the current pane size.
    bool fits_pane(const PageBmp* c) const {
        RECT a;
        image_area(&a);
        int w = a.right - a.left, h = a.bottom - a.top;
        return c->req_w >= w * 9 / 10 && c->req_h >= h * 9 / 10 && c->req_w <= w * 11 / 10 + 1 &&
               c->req_h <= h * 11 / 10 + 1;
    }

    // ---- requests

    void request(bool want_pixels) {
        if (!m_wnd || !m_path || !m_shared) return;
        RECT a;
        image_area(&a);
        int w = a.right - a.left, h = a.bottom - a.top;
        if (w < 16 || h < 16) return;
        ++m_gen;
        if (want_pixels) {
            m_req_w = w;
            m_req_h = h;
        }
        AcquireSRWLockExclusive(&m_shared->lock);
        m_shared->gen = m_gen;
        m_shared->file_gen = m_file_gen;
        m_shared->cx = w;
        m_shared->cy = h;
        m_shared->page = m_page;
        m_shared->count = page_count();
        m_shared->dir = m_dir;
        m_shared->want_pixels = want_pixels;
        m_shared->has_req = true;
        abt::mem_free(m_shared->path);
        size_t n = (size_t)lstrlenW(m_path) + 1;
        m_shared->path = (wchar_t*)abt::mem_alloc(n * sizeof(wchar_t));
        if (m_shared->path) memcpy(m_shared->path, m_path, n * sizeof(wchar_t));
        ReleaseSRWLockExclusive(&m_shared->lock);
        SetEvent(m_shared->wake);
    }

    void on_result(Result* r) {
        if (r->file_gen != m_file_gen) { free_result(r); return; }
        bool current = r->gen == m_gen && r->page == m_page;
        if (r->info.family && (!r->quick || !m_have_info)) {
            uint32_t pages = m_have_info ? m_info.page_count : 0;
            m_info = r->info;
            if (r->quick && pages > m_info.page_count) m_info.page_count = pages;
            m_have_info = true;
        }
        if (r->quick) {
            // The thumbnail is the whole document; for an Illustrator file with several artboards or
            // art outside artboard 1 it is not artboard 1, so it must not stand in for it.
            bool same_picture = !(r->info.flags & ABT_INFO_CANVAS_BEYOND_PAGE1);
            if (current && SUCCEEDED(r->hr) && same_picture && (!m_bmp || m_quick) && m_loading) {
                if (m_bmp && m_quick) DeleteObject(m_bmp);
                void* bits = nullptr;
                m_bmp = abt::create_dib32(r->w, r->h, &bits);
                if (m_bmp) {
                    memcpy(bits, r->px, (size_t)r->w * r->h * 4);
                    m_bw = r->w;
                    m_bh = r->h;
                    m_alpha = false;
                    for (size_t i = 0, n = (size_t)r->w * r->h; i < n; ++i)
                        if (r->px[i * 4 + 3] != 255) { m_alpha = true; break; }
                    m_quick = true;
                }
            }
        } else if (SUCCEEDED(r->hr)) {
            PageBmp* c = cache_put(r);  // keep every finished page, current or not
            if (c && (current || (r->page == m_page && m_loading))) {
                show(c);
                m_loading = false;
                m_error[0] = 0;
            }
        } else if (current) {
            // Never leave another artboard on screen under this artboard's number.
            if (m_bmp && m_quick) DeleteObject(m_bmp);
            m_bmp = nullptr;
            m_quick = false;
            m_loading = false;
            lstrcpynA(m_error, r->reason[0] ? r->reason : "No preview available", sizeof(m_error));
        }
        free_result(r);
        InvalidateRect(m_wnd, nullptr, FALSE);
    }

    void go_page(int p) {
        int n = page_count();
        if (p < 0) p = 0;
        if (p >= n) p = n - 1;
        if (p == m_page) return;
        m_dir = p > m_page ? 1 : -1;
        m_page = p;
        m_error[0] = 0;
        PageBmp* c = cache_find(p);
        if (c && fits_pane(c)) {
            show(c);  // instant; still let the agent warm up the new neighbours
            m_loading = false;
            request(false);
        } else {
            m_loading = true;  // keep the previous page on screen, dimmed, until the new one arrives
            request(true);
        }
        InvalidateRect(m_wnd, nullptr, FALSE);
    }

    void info_text(wchar_t* out, size_t cap) {
        out[0] = 0;
        if (!m_have_info) return;
        wchar_t size[64] = L"";
        if (m_info.px_width)
            _snwprintf_s(size, _countof(size), _TRUNCATE, L"  \x00b7  %u \x00d7 %u px", m_info.px_width, m_info.px_height);
        else if (m_info.pt_width > 0)
            _snwprintf_s(size, _countof(size), _TRUNCATE, L"  \x00b7  %.0f \x00d7 %.0f mm", m_info.pt_width * 25.4 / 72.0,
                         m_info.pt_height * 25.4 / 72.0);
        wchar_t cm[32] = L"";
        const char* cms = abt_color_mode_str((abt_color_mode)m_info.color_mode);
        if (cms && *cms) {
            if (m_info.bit_depth) _snwprintf_s(cm, _countof(cm), _TRUNCATE, L"  \x00b7  %hs %u-bit", cms, m_info.bit_depth);
            else _snwprintf_s(cm, _countof(cm), _TRUNCATE, L"  \x00b7  %hs", cms);
        }
        _snwprintf_s(out, cap, _TRUNCATE, L"%hs%s%s", abt_family_str((abt_family)m_info.family), size, cm);
    }

    const wchar_t* page_noun() const { return m_info.family == ABT_FAM_AI_PDF ? L"Artboard" : L"Page"; }

    // ---- painting

    void draw_image(HDC mdc, const RECT& area) {
        int aw = area.right - area.left, ah = area.bottom - area.top;
        if (!m_bmp || aw <= 0 || ah <= 0) return;
        // Fit into the area; the final image is never enlarged beyond 1:1 device pixels (it was
        // rendered for this size), the quick first paint may be.
        double s = fmin((double)aw / m_bw, (double)ah / m_bh);
        if (!m_quick && s > 1.0) s = 1.0;
        int dw = (int)(m_bw * s + 0.5), dh = (int)(m_bh * s + 0.5);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        RECT dst = {area.left + (aw - dw) / 2, area.top + (ah - dh) / 2, 0, 0};
        dst.right = dst.left + dw;
        dst.bottom = dst.top + dh;
        if (m_alpha && m_checker) {
            SetBrushOrgEx(mdc, dst.left, dst.top, nullptr);
            FillRect(mdc, &dst, m_checker);
        }
        HDC idc = CreateCompatibleDC(mdc);
        HGDIOBJ oi = SelectObject(idc, m_bmp);
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, (BYTE)(m_loading ? 110 : 255), AC_SRC_ALPHA};
        AlphaBlend(mdc, dst.left, dst.top, dw, dh, idc, 0, 0, m_bw, m_bh, bf);
        SelectObject(idc, oi);
        DeleteDC(idc);
    }

    void draw_center_text(HDC mdc, const RECT& area, const wchar_t* text, COLORREF c) {
        RECT r = {area.left + px(16), area.top, area.right - px(16), area.bottom};
        RECT m = r;
        ::SetTextColor(mdc, c);
        DrawTextW(mdc, text, -1, &m, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
        int th = m.bottom - m.top;
        r.top = area.top + (area.bottom - area.top - th) / 2;
        r.bottom = r.top + th;
        DrawTextW(mdc, text, -1, &r, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
    }

    void draw_glyph(HDC mdc, HFONT f, wchar_t g, const RECT& r, COLORREF c) {
        HGDIOBJ of = SelectObject(mdc, f);
        ::SetTextColor(mdc, c);
        RECT t = r;
        DrawTextW(mdc, &g, 1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        SelectObject(mdc, of);
    }

    void paint(HDC dc) {
        RECT rc;
        GetClientRect(m_wnd, &rc);
        int w = rc.right, h = rc.bottom;
        if (w <= 0 || h <= 0) return;
        ensure_gdi();
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP back = CreateCompatibleBitmap(dc, w, h);
        HGDIOBJ oldb = SelectObject(mdc, back);
        fill(mdc, rc, m_bg);
        SetBkMode(mdc, TRANSPARENT);
        HGDIOBJ of = SelectObject(mdc, m_font);
        COLORREF dim = mix(m_bg, m_fg, 55), faint = mix(m_bg, m_fg, 10), line = mix(m_bg, m_fg, 14);
        int n = page_count();

        RECT area;
        image_area(&area);
        draw_image(mdc, area);
        wchar_t msg[160];
        if (!m_bmp) {
            if (m_error[0]) {
                wchar_t err[100];
                MultiByteToWideChar(CP_UTF8, 0, m_error, -1, err, _countof(err));
                if (n > 1) _snwprintf_s(msg, _countof(msg), _TRUNCATE, L"%s %d: %s", page_noun(), m_page + 1, err);
                else lstrcpynW(msg, err, _countof(msg));
                draw_center_text(mdc, area, msg, dim);
            } else if (m_path) {
                if (n > 1) _snwprintf_s(msg, _countof(msg), _TRUNCATE, L"Rendering %s %d\x2026", page_noun(), m_page + 1);
                else lstrcpynW(msg, L"Rendering\x2026", _countof(msg));
                draw_center_text(mdc, area, msg, dim);
            }
        } else if (m_loading) {
            // Previous page stays on screen dimmed; say what is coming.
            _snwprintf_s(msg, _countof(msg), _TRUNCATE, L"Rendering %s %d\x2026", page_noun(), m_page + 1);
            SIZE ts = {};
            GetTextExtentPoint32W(mdc, msg, lstrlenW(msg), &ts);
            RECT pill = {0, area.top + px(10), 0, area.top + px(10) + ts.cy + px(8)};
            pill.left = (area.left + area.right) / 2 - ts.cx / 2 - px(10);
            pill.right = pill.left + ts.cx + px(20);
            round_fill(mdc, pill, px(14), mix(m_bg, m_fg, 8));
            ::SetTextColor(mdc, m_fg);
            DrawTextW(mdc, msg, -1, &pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        // Hover arrows over the image (multi-page files only).
        if (n > 1 && (m_zone == Z_IMG_PREV || m_zone == Z_IMG_NEXT)) {
            RECT ap, an;
            image_arrows(&ap, &an);
            bool prev = m_zone == Z_IMG_PREV;
            if (can_go(prev ? -1 : 1)) {
                const RECT& r = prev ? ap : an;
                COLORREF disc = is_dark(m_bg) ? RGB(32, 32, 32) : RGB(250, 250, 250);
                HBRUSH b = CreateSolidBrush(disc);
                HPEN p = CreatePen(PS_SOLID, 1, mix(disc, m_fg, 25));
                HGDIOBJ ob = SelectObject(mdc, b), op = SelectObject(mdc, p);
                Ellipse(mdc, r.left, r.top, r.right, r.bottom);
                SelectObject(mdc, op);
                SelectObject(mdc, ob);
                DeleteObject(p);
                DeleteObject(b);
                draw_glyph(mdc, m_icons_big, prev ? 0xE76B : 0xE76C, r, m_fg);
            }
        }

        // Info strip: separator, file facts on the left, page navigation on the right.
        int sh = strip_h();
        RECT sep = {0, h - sh, w, h - sh + 1};
        fill(mdc, sep, line);
        RECT tr = {px(10), h - sh, w - px(10), h};
        if (n > 1) {
            RECT bp, bl, bn;
            strip_buttons(&bp, &bl, &bn);
            tr.right = bp.left - px(8);
            bool cp = can_go(-1), cn = can_go(1);
            if (m_zone == Z_BTN_PREV && cp) round_fill(mdc, bp, px(6), faint);
            if (m_zone == Z_BTN_NEXT && cn) round_fill(mdc, bn, px(6), faint);
            draw_glyph(mdc, m_icons, 0xE76B, bp, cp ? m_fg : mix(m_bg, m_fg, 30));
            draw_glyph(mdc, m_icons, 0xE76C, bn, cn ? m_fg : mix(m_bg, m_fg, 30));
            wchar_t lab[32];
            _snwprintf_s(lab, _countof(lab), _TRUNCATE, L"%d / %d", m_page + 1, n);
            ::SetTextColor(mdc, m_fg);
            DrawTextW(mdc, lab, -1, &bl, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        wchar_t text[200];
        info_text(text, _countof(text));
        if (n > 1) {
            size_t l = wcslen(text);
            _snwprintf_s(text + l, _countof(text) - l, _TRUNCATE, L"%s%s %d", l ? L"  \x00b7  " : L"", page_noun(),
                         m_page + 1);
        }
        ::SetTextColor(mdc, dim);
        DrawTextW(mdc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(mdc, of);
        BitBlt(dc, 0, 0, w, h, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, oldb);
        DeleteObject(back);
        DeleteDC(mdc);
    }

    // ---- input

    int zone_at(int x, int y) const {
        if (page_count() < 2) return Z_NONE;
        RECT bp, bl, bn;
        strip_buttons(&bp, &bl, &bn);
        if (in_rect(bp, x, y)) return Z_BTN_PREV;
        if (in_rect(bn, x, y)) return Z_BTN_NEXT;
        RECT a;
        image_area(&a);
        if (!in_rect(a, x, y)) return Z_NONE;
        int band = (a.right - a.left) / 4;
        if (band > px(96)) band = px(96);
        if (x < a.left + band) return Z_IMG_PREV;
        if (x >= a.right - band) return Z_IMG_NEXT;
        return Z_NONE;
    }
    void set_zone(int z) {
        if (z == m_zone) return;
        m_zone = z;
        InvalidateRect(m_wnd, nullptr, FALSE);
    }
    void click(int z) {
        if (z == Z_IMG_PREV || z == Z_BTN_PREV) go_page(m_page - 1);
        else if (z == Z_IMG_NEXT || z == Z_BTN_NEXT) go_page(m_page + 1);
    }

    void on_resize_settled() {
        RECT a;
        image_area(&a);
        int w = a.right - a.left, h = a.bottom - a.top;
        // Re-render when the pane changed noticeably either way: growing needs more pixels, and
        // shrinking a large image with AlphaBlend looks rough. Smaller sizes come from the agent's
        // cache by downscaling, so this is cheap.
        bool grow = w > m_req_w * 11 / 10 || h > m_req_h * 11 / 10;
        bool shrink = w < m_req_w * 9 / 10 && h < m_req_h * 9 / 10;
        if (grow || shrink) {
            PageBmp* c = cache_find(m_page);
            if (c && fits_pane(c)) {
                show(c);
                m_req_w = w;
                m_req_h = h;
                InvalidateRect(m_wnd, nullptr, FALSE);
            } else {
                request(true);
            }
        }
    }

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        PreviewHandler* self = (PreviewHandler*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (msg == WM_NCCREATE) {
            self = (PreviewHandler*)((CREATESTRUCTW*)lp)->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            self->paint(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            self->paint((HDC)wp);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_APP_RESULT:
            self->on_result((Result*)lp);
            return 0;
        case WM_SIZE:
            InvalidateRect(hwnd, nullptr, FALSE);
            SetTimer(hwnd, kResizeTimer, 150, nullptr);  // debounce re-render while dragging
            return 0;
        case WM_DPICHANGED_AFTERPARENT:
            InvalidateRect(hwnd, nullptr, FALSE);
            SetTimer(hwnd, kResizeTimer, 50, nullptr);
            return 0;
        case WM_TIMER:
            if (wp == kResizeTimer) {
                KillTimer(hwnd, kResizeTimer);
                self->on_resize_settled();
            }
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        case WM_KEYDOWN:
            switch (wp) {
            case VK_RIGHT: case VK_DOWN: case VK_NEXT: self->go_page(self->m_page + 1); return 0;
            case VK_LEFT: case VK_UP: case VK_PRIOR: self->go_page(self->m_page - 1); return 0;
            case VK_HOME: self->go_page(0); return 0;
            case VK_END: self->go_page(self->page_count() - 1); return 0;
            }
            break;
        case WM_MOUSEWHEEL: {
            // Precision touchpads send many small deltas: one page per full notch.
            self->m_wheel += GET_WHEEL_DELTA_WPARAM(wp);
            while (self->m_wheel >= WHEEL_DELTA) { self->m_wheel -= WHEEL_DELTA; self->go_page(self->m_page - 1); }
            while (self->m_wheel <= -WHEEL_DELTA) { self->m_wheel += WHEEL_DELTA; self->go_page(self->m_page + 1); }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (!self->m_tracking) {
                TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, hwnd, 0};
                self->m_tracking = TrackMouseEvent(&t) != 0;
            }
            self->set_zone(self->zone_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
            return 0;
        case WM_MOUSELEAVE:
            self->m_tracking = false;
            self->set_zone(Z_NONE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                int z = self->m_zone;
                bool active = (z == Z_IMG_PREV || z == Z_BTN_PREV) ? self->can_go(-1)
                              : (z == Z_IMG_NEXT || z == Z_BTN_NEXT) ? self->can_go(1)
                                                                     : false;
                SetCursor(LoadCursorW(nullptr, active ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN:
            ::SetFocus(hwnd);
            self->click(self->zone_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static void ensure_class() {
        WNDCLASSEXW wc = {sizeof(wc)};
        if (GetClassInfoExW(g_dll, kWndClass, &wc)) return;
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = g_dll;
        wc.hCursor = nullptr;  // WM_SETCURSOR picks arrow or hand
        wc.lpszClassName = kWndClass;
        RegisterClassExW(&wc);
    }

    void stop_thread() {
        if (!m_shared) return;
        AcquireSRWLockExclusive(&m_shared->lock);
        m_shared->stop = true;
        m_shared->notify = nullptr;
        ReleaseSRWLockExclusive(&m_shared->lock);
        SetEvent(m_shared->wake);
        shared_release(m_shared);  // the thread holds its own reference
        m_shared = nullptr;
        // Free results that were posted before notify was cleared but never delivered.
        MSG m;
        while (m_wnd && PeekMessageW(&m, m_wnd, WM_APP_RESULT, WM_APP_RESULT, PM_REMOVE)) free_result((Result*)m.lParam);
    }

    void reset_file() {
        CoTaskMemFree(m_path);
        m_path = nullptr;
        cache_clear();
        m_have_info = false;
        m_info = {};
        m_page = 0;
        m_dir = 1;
        m_error[0] = 0;
        m_loading = false;
        m_req_w = m_req_h = 0;
        ++m_file_gen;  // results still in flight for the previous file are dropped
    }

public:
    PreviewHandler() { dll_addref(); }
    ~PreviewHandler() {
        stop_thread();
        if (m_wnd) DestroyWindow(m_wnd);
        if (m_site) m_site->Release();
        reset_file();
        free_gdi();
        dll_release();
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IPreviewHandler) *ppv = static_cast<IPreviewHandler*>(this);
        else if (riid == IID_IPreviewHandlerVisuals) *ppv = static_cast<IPreviewHandlerVisuals*>(this);
        else if (riid == IID_IObjectWithSite) *ppv = static_cast<IObjectWithSite*>(this);
        else if (riid == IID_IOleWindow) *ppv = static_cast<IOleWindow*>(this);
        else if (riid == IID_IInitializeWithItem) *ppv = static_cast<IInitializeWithItem*>(this);
        else if (riid == IID_IInitializeWithFile) *ppv = static_cast<IInitializeWithFile*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return (ULONG)r;
    }

    IFACEMETHODIMP Initialize(IShellItem* item, DWORD) override {
        reset_file();
        return item->GetDisplayName(SIGDN_FILESYSPATH, &m_path);
    }
    IFACEMETHODIMP Initialize(LPCWSTR path, DWORD) override {
        reset_file();
        return SHStrDupW(path, &m_path);
    }

    IFACEMETHODIMP SetWindow(HWND parent, const RECT* rc) override {
        m_parent = parent;
        if (rc) m_rect = *rc;
        if (m_wnd) {
            SetParent(m_wnd, m_parent);
            SetWindowPos(m_wnd, nullptr, m_rect.left, m_rect.top, m_rect.right - m_rect.left,
                         m_rect.bottom - m_rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return S_OK;
    }
    IFACEMETHODIMP SetRect(const RECT* rc) override {
        if (!rc) return E_INVALIDARG;
        m_rect = *rc;
        if (m_wnd)
            SetWindowPos(m_wnd, nullptr, m_rect.left, m_rect.top, m_rect.right - m_rect.left,
                         m_rect.bottom - m_rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
        return S_OK;
    }
    IFACEMETHODIMP DoPreview() override {
        if (!m_parent || !m_path) return E_FAIL;
        if (!m_wnd) {
            ensure_class();
            m_wnd = CreateWindowExW(0, kWndClass, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, m_rect.left,
                                    m_rect.top, m_rect.right - m_rect.left, m_rect.bottom - m_rect.top, m_parent,
                                    nullptr, g_dll, this);
            if (!m_wnd) return HRESULT_FROM_WIN32(GetLastError());
        }
        if (!m_shared) {
            m_shared = (Shared*)abt::mem_zalloc(sizeof(Shared));
            if (!m_shared) return E_OUTOFMEMORY;
            m_shared->refs = 2;
            InitializeSRWLock(&m_shared->lock);
            m_shared->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            m_shared->notify = m_wnd;
            HMODULE pin = nullptr;  // released by the thread (FreeLibraryAndExitThread)
            bool pinned = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&request_thread, &pin) != 0;
            HANDLE t = m_shared->wake && pinned ? CreateThread(nullptr, 0, request_thread, m_shared, 0, nullptr) : nullptr;
            if (t) CloseHandle(t);
            else {
                if (pin) FreeLibrary(pin);
                shared_release(m_shared);
                shared_release(m_shared);
                m_shared = nullptr;
                return E_FAIL;
            }
        }
        m_loading = true;
        request(true);
        InvalidateRect(m_wnd, nullptr, FALSE);
        return S_OK;
    }
    IFACEMETHODIMP Unload() override {
        stop_thread();
        if (m_wnd) { DestroyWindow(m_wnd); m_wnd = nullptr; }
        reset_file();
        return S_OK;
    }
    IFACEMETHODIMP SetFocus() override {
        if (m_wnd) ::SetFocus(m_wnd);
        return S_OK;
    }
    IFACEMETHODIMP QueryFocus(HWND* out) override {
        *out = GetFocus();
        return *out ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    IFACEMETHODIMP TranslateAccelerator(MSG* msg) override {
        IPreviewHandlerFrame* frame = nullptr;
        HRESULT hr = S_FALSE;
        if (m_site && SUCCEEDED(m_site->QueryInterface(IID_PPV_ARGS(&frame)))) {
            hr = frame->TranslateAccelerator(msg);
            frame->Release();
        }
        return hr;
    }

    IFACEMETHODIMP SetBackgroundColor(COLORREF c) override {
        m_bg = c;
        if (m_wnd) InvalidateRect(m_wnd, nullptr, FALSE);
        return S_OK;
    }
    IFACEMETHODIMP SetFont(const LOGFONTW*) override { return S_OK; }
    IFACEMETHODIMP SetTextColor(COLORREF c) override {
        m_fg = c;
        if (m_wnd) InvalidateRect(m_wnd, nullptr, FALSE);
        return S_OK;
    }

    IFACEMETHODIMP SetSite(IUnknown* site) override {
        if (m_site) m_site->Release();
        m_site = site;
        if (m_site) m_site->AddRef();
        return S_OK;
    }
    IFACEMETHODIMP GetSite(REFIID riid, void** ppv) override {
        *ppv = nullptr;
        return m_site ? m_site->QueryInterface(riid, ppv) : E_FAIL;
    }

    IFACEMETHODIMP GetWindow(HWND* out) override {
        *out = m_wnd;
        return m_wnd ? S_OK : E_FAIL;
    }
    IFACEMETHODIMP ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
};

}  // namespace

HRESULT create_preview_handler(REFIID riid, void** ppv) {
    abt::log_init(L"shellext");
    PreviewHandler* p = new (std::nothrow) PreviewHandler();
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release();
    return hr;
}
