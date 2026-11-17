// IPreviewHandler (step 3f): fit-to-pane image rendered by the agent at device pixels, quick
// low-res first paint from the thumbnail cache, re-render on resize, page / artboard navigation
// (arrow keys, wheel, click), info strip. Rendering requests run on a background thread.
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

// Shared between the handler (UI thread) and its request thread; refcounted so the thread can
// finish a slow request after the handler is gone.
struct Shared {
    volatile LONG refs;
    SRWLOCK lock;
    HANDLE wake;
    bool stop;
    // pending request
    bool has_req;
    int gen, cx, cy, page;
    wchar_t* path;
    HWND notify;
};

struct Result {
    int gen;
    bool quick;  // low-res first paint
    HRESULT hr;
    int w, h;
    uint8_t* px;  // premultiplied BGRA
    abt_file_info info;
    char reason[100];
};

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
    if (!w || !PostMessageW(w, WM_APP_RESULT, 0, (LPARAM)r)) {
        abt::mem_free(r->px);
        abt::mem_free(r);
    }
}

Result* fetch(uint16_t msg, const wchar_t* path, int cx, int cy, int page, int gen, bool quick) {
    Result* r = (Result*)abt::mem_zalloc(sizeof(Result));
    if (!r) return nullptr;
    abt_resp resp;
    r->gen = gen;
    r->quick = quick;
    r->hr = request_image(msg, path, (UINT)cx, (UINT)cy, (UINT)page, &resp, &r->px);
    r->w = (int)resp.width;
    r->h = (int)resp.height;
    r->info = resp.info;
    memcpy(r->reason, resp.reason, sizeof(r->reason));
    if (FAILED(r->hr)) { abt::mem_free(r->px); r->px = nullptr; }
    return r;
}

DWORD WINAPI request_thread(void* arg) {
    Shared* s = (Shared*)arg;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int done_gen = -1;
    for (;;) {
        WaitForSingleObject(s->wake, INFINITE);
        AcquireSRWLockExclusive(&s->lock);
        bool stop = s->stop, has = s->has_req;
        int gen = s->gen, cx = s->cx, cy = s->cy, page = s->page;
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
        // First paint of a new file: the thumbnail tier is usually cached already (sub-ms).
        if (done_gen < 0 && page == 0) {
            if (Result* q = fetch(ABT_MSG_THUMB, path, 256, 256, 0, gen, true)) post_result(s, q);
        }
        if (Result* r = fetch(ABT_MSG_PREVIEW, path, cx, cy, page, gen, false)) post_result(s, r);
        done_gen = gen;
        abt::mem_free(path);
    }
    CoUninitialize();
    shared_release(s);
    return 0;
}

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
    // displayed state
    HBITMAP m_bmp = nullptr;
    int m_bw = 0, m_bh = 0;
    bool m_quick = false;
    abt_file_info m_info = {};
    bool m_have_info = false;
    int m_page = 0;
    int m_gen = 0;
    int m_req_w = 0, m_req_h = 0;
    char m_error[100] = "";

    int dpi() const { return m_wnd ? (int)GetDpiForWindow(m_wnd) : 96; }
    int strip_h() const { return MulDiv(26, dpi(), 96); }
    int page_count() const { return m_have_info && m_info.page_count > 1 ? (int)m_info.page_count : 1; }

    void request(bool new_gen) {
        if (!m_wnd || !m_path || !m_shared) return;
        RECT rc;
        GetClientRect(m_wnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top - strip_h();
        if (w < 16 || h < 16) return;
        if (new_gen) ++m_gen;
        m_req_w = w;
        m_req_h = h;
        AcquireSRWLockExclusive(&m_shared->lock);
        m_shared->gen = m_gen;
        m_shared->cx = w;
        m_shared->cy = h;
        m_shared->page = m_page;
        m_shared->has_req = true;
        abt::mem_free(m_shared->path);
        size_t n = (size_t)lstrlenW(m_path) + 1;
        m_shared->path = (wchar_t*)abt::mem_alloc(n * sizeof(wchar_t));
        if (m_shared->path) memcpy(m_shared->path, m_path, n * sizeof(wchar_t));
        ReleaseSRWLockExclusive(&m_shared->lock);
        SetEvent(m_shared->wake);
    }

    void set_image(Result* r) {
        if (m_bmp) { DeleteObject(m_bmp); m_bmp = nullptr; }
        void* bits = nullptr;
        m_bmp = abt::create_dib32(r->w, r->h, &bits);
        if (m_bmp) {
            memcpy(bits, r->px, (size_t)r->w * r->h * 4);  // premultiplied: ready for AlphaBlend
            m_bw = r->w;
            m_bh = r->h;
        }
    }

    void on_result(Result* r) {
        if (r->gen == m_gen) {
            if (SUCCEEDED(r->hr) && r->px && (!r->quick || !m_bmp || m_quick)) {
                set_image(r);
                m_quick = r->quick;
                m_error[0] = 0;
            } else if (FAILED(r->hr) && !r->quick && !m_bmp) {
                lstrcpynA(m_error, r->reason[0] ? r->reason : "No preview available", sizeof(m_error));
            }
            if (!r->quick || !m_have_info) {
                if (r->info.family) { m_info = r->info; m_have_info = true; }
            }
            InvalidateRect(m_wnd, nullptr, FALSE);
        }
        abt::mem_free(r->px);
        abt::mem_free(r);
    }

    void go_page(int delta) {
        int n = page_count();
        int p = m_page + delta;
        if (p < 0 || p >= n) return;
        m_page = p;
        request(true);
        InvalidateRect(m_wnd, nullptr, FALSE);
    }

    void info_text(wchar_t* out, size_t cap) {
        if (!m_have_info) { out[0] = 0; return; }
        wchar_t size[64] = L"";
        if (m_info.px_width) _snwprintf_s(size, _countof(size), _TRUNCATE, L"%u \x00d7 %u px", m_info.px_width, m_info.px_height);
        else if (m_info.pt_width > 0)
            _snwprintf_s(size, _countof(size), _TRUNCATE, L"%.0f \x00d7 %.0f mm", m_info.pt_width * 25.4 / 72.0,
                         m_info.pt_height * 25.4 / 72.0);
        wchar_t cm[32] = L"";
        const char* cms = abt_color_mode_str((abt_color_mode)m_info.color_mode);
        if (cms && *cms) {
            if (m_info.bit_depth) _snwprintf_s(cm, _countof(cm), _TRUNCATE, L"  \x00b7  %hs %u-bit", cms, m_info.bit_depth);
            else _snwprintf_s(cm, _countof(cm), _TRUNCATE, L"  \x00b7  %hs", cms);
        }
        wchar_t pages[48] = L"";
        if (page_count() > 1)
            _snwprintf_s(pages, _countof(pages), _TRUNCATE, L"  \x00b7  %s %d / %d   \x25C0 \x25B6",
                         m_info.family == ABT_FAM_AI_PDF ? L"artboard" : L"page", m_page + 1, page_count());
        _snwprintf_s(out, cap, _TRUNCATE, L"%hs  \x00b7  %s%s%s", abt_family_str((abt_family)m_info.family), size, cm, pages);
    }

    void paint(HDC dc) {
        RECT rc;
        GetClientRect(m_wnd, &rc);
        int w = rc.right, h = rc.bottom;
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP back = CreateCompatibleBitmap(dc, w > 0 ? w : 1, h > 0 ? h : 1);
        HGDIOBJ oldb = SelectObject(mdc, back);
        HBRUSH br = CreateSolidBrush(m_bg);
        FillRect(mdc, &rc, br);
        DeleteObject(br);
        int sh = strip_h();
        int area_h = h - sh;
        if (m_bmp && area_h > 0) {
            // Fit the image (which may be a quick low-res one) into the area, never upscaling the
            // final image beyond 1:1 device pixels.
            double s = fmin((double)w / m_bw, (double)area_h / m_bh);
            if (!m_quick && s > 1.0) s = 1.0;
            int dw = (int)(m_bw * s + 0.5), dh = (int)(m_bh * s + 0.5);
            int x = (w - dw) / 2, y = (area_h - dh) / 2;
            HDC idc = CreateCompatibleDC(dc);
            HGDIOBJ oi = SelectObject(idc, m_bmp);
            SetStretchBltMode(mdc, HALFTONE);
            BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            AlphaBlend(mdc, x, y, dw, dh, idc, 0, 0, m_bw, m_bh, bf);
            SelectObject(idc, oi);
            DeleteDC(idc);
        }
        SetBkMode(mdc, TRANSPARENT);
        ::SetTextColor(mdc, m_fg);
        HFONT f = CreateFontW(-MulDiv(12, dpi(), 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                              CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ of = SelectObject(mdc, f);
        wchar_t text[256];
        if (m_error[0] && !m_bmp) {
            wchar_t err[128];
            MultiByteToWideChar(CP_UTF8, 0, m_error, -1, err, 128);
            RECT er = {0, 0, w, area_h};
            DrawTextW(mdc, err, -1, &er, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        info_text(text, _countof(text));
        RECT tr = {MulDiv(8, dpi(), 96), area_h, w - MulDiv(8, dpi(), 96), h};
        DrawTextW(mdc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(mdc, of);
        DeleteObject(f);
        BitBlt(dc, 0, 0, w, h, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, oldb);
        DeleteObject(back);
        DeleteDC(mdc);
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
        case WM_TIMER:
            if (wp == kResizeTimer) {
                KillTimer(hwnd, kResizeTimer);
                RECT rc;
                GetClientRect(hwnd, &rc);
                int w = rc.right, h = rc.bottom - self->strip_h();
                // Re-render only when the pane outgrows the current image noticeably.
                if (w > self->m_req_w * 11 / 10 || h > self->m_req_h * 11 / 10) self->request(false);
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_RIGHT || wp == VK_NEXT || wp == VK_DOWN) { self->go_page(1); return 0; }
            if (wp == VK_LEFT || wp == VK_PRIOR || wp == VK_UP) { self->go_page(-1); return 0; }
            break;
        case WM_MOUSEWHEEL:
            self->go_page(GET_WHEEL_DELTA_WPARAM(wp) < 0 ? 1 : -1);
            return 0;
        case WM_LBUTTONDOWN: {
            ::SetFocus(hwnd);
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (self->page_count() > 1) self->go_page(GET_X_LPARAM(lp) > rc.right / 2 ? 1 : -1);
            return 0;
        }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static void ensure_class() {
        WNDCLASSEXW wc = {sizeof(wc)};
        if (GetClassInfoExW(g_dll, kWndClass, &wc)) return;
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = g_dll;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
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
    }

    void reset_file() {
        CoTaskMemFree(m_path);
        m_path = nullptr;
        if (m_bmp) { DeleteObject(m_bmp); m_bmp = nullptr; }
        m_have_info = false;
        m_page = 0;
        m_error[0] = 0;
        m_quick = false;
    }

public:
    PreviewHandler() { dll_addref(); }
    ~PreviewHandler() {
        stop_thread();
        if (m_wnd) DestroyWindow(m_wnd);
        if (m_site) m_site->Release();
        reset_file();
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
            HANDLE t = CreateThread(nullptr, 0, request_thread, m_shared, 0, nullptr);
            if (t) CloseHandle(t);
            else { shared_release(m_shared); shared_release(m_shared); m_shared = nullptr; return E_FAIL; }
        }
        request(true);
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
