// IPreviewHandler. Step 2: placeholder window + lifecycle logging.
#include "shellext.h"
#include "abt_wic.h"
#include <shobjidl.h>
#include <shlwapi.h>
#include <ocidl.h>

namespace {

const wchar_t kWndClass[] = L"abthumbPreviewWnd";

class PreviewHandler final : public IPreviewHandler,
                             public IPreviewHandlerVisuals,
                             public IObjectWithSite,
                             public IOleWindow,
                             public IInitializeWithItem,
                             public IInitializeWithFile,
                             public IInitializeWithStream {
    LONG m_ref = 1;
    HWND m_parent = nullptr;
    HWND m_wnd = nullptr;
    RECT m_rect = {};
    IUnknown* m_site = nullptr;
    wchar_t* m_path = nullptr;
    IStream* m_stream = nullptr;
    COLORREF m_bg = RGB(255, 255, 255);
    COLORREF m_fg = RGB(0, 0, 0);

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        PreviewHandler* self = (PreviewHandler*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (msg == WM_NCCREATE) {
            self = (PreviewHandler*)((CREATESTRUCTW*)lp)->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        }
        if (msg == WM_PAINT && self) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left, h = rc.bottom - rc.top;
            if (w > 0 && h > 0) {
                int side = w < h ? w : h;
                void* bits = nullptr;
                HBITMAP bmp = abt::create_dib32(side, side, &bits);
                if (bmp) {
                    wchar_t line2[64];
                    _snwprintf_s(line2, _countof(line2), _TRUNCATE, L"preview %dx%d", w, h);
                    draw_placeholder(bits, side, side, self->m_path ? self->m_path : L"stream", line2);
                    HBRUSH br = CreateSolidBrush(self->m_bg);
                    FillRect(dc, &rc, br);
                    DeleteObject(br);
                    HDC mdc = CreateCompatibleDC(dc);
                    HGDIOBJ old = SelectObject(mdc, bmp);
                    BitBlt(dc, (w - side) / 2, (h - side) / 2, side, side, mdc, 0, 0, SRCCOPY);
                    SelectObject(mdc, old);
                    DeleteDC(mdc);
                    DeleteObject(bmp);
                }
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        if (msg == WM_ERASEBKGND) return 1;
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

    void reset() {
        if (m_stream) { m_stream->Release(); m_stream = nullptr; }
        CoTaskMemFree(m_path);
        m_path = nullptr;
    }

public:
    PreviewHandler() { dll_addref(); }
    ~PreviewHandler() {
        if (m_wnd) DestroyWindow(m_wnd);
        if (m_site) m_site->Release();
        reset();
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
        else if (riid == IID_IInitializeWithStream) *ppv = static_cast<IInitializeWithStream*>(this);
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

    // Initialization
    IFACEMETHODIMP Initialize(IShellItem* item, DWORD) override {
        reset();
        HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &m_path);
        abt::logf("preview Initialize(item) hr=0x%08lx path=%ls", hr, m_path ? m_path : L"(null)");
        return hr;
    }
    IFACEMETHODIMP Initialize(LPCWSTR path, DWORD) override {
        reset();
        abt::logf("preview Initialize(file) path=%ls", path);
        return SHStrDupW(path, &m_path);
    }
    IFACEMETHODIMP Initialize(IStream* stream, DWORD) override {
        reset();
        m_stream = stream;
        stream->AddRef();
        abt::logf("preview Initialize(stream)");
        return S_OK;
    }

    // IPreviewHandler
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
        if (m_wnd) return S_OK;
        if (!m_parent) return E_FAIL;
        ensure_class();
        m_wnd = CreateWindowExW(0, kWndClass, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, m_rect.left,
                                m_rect.top, m_rect.right - m_rect.left, m_rect.bottom - m_rect.top, m_parent,
                                nullptr, g_dll, this);
        abt::logf("preview DoPreview hwnd=%p rect=%ldx%ld", m_wnd, m_rect.right - m_rect.left,
                  m_rect.bottom - m_rect.top);
        return m_wnd ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    IFACEMETHODIMP Unload() override {
        if (m_wnd) { DestroyWindow(m_wnd); m_wnd = nullptr; }
        reset();
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

    // IPreviewHandlerVisuals
    IFACEMETHODIMP SetBackgroundColor(COLORREF c) override { m_bg = c; if (m_wnd) InvalidateRect(m_wnd, nullptr, FALSE); return S_OK; }
    IFACEMETHODIMP SetFont(const LOGFONTW*) override { return S_OK; }
    IFACEMETHODIMP SetTextColor(COLORREF c) override { m_fg = c; return S_OK; }

    // IObjectWithSite
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

    // IOleWindow
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
