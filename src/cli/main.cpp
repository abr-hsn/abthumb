// abthumb.exe: registration, shell tests, (later) install, render, bench, stats.
#include "abt_win.h"
#include "abt_registry.h"
#include "abt_wic.h"
#include "abt_proto.h"
#include "render.h"
#include <shobjidl.h>
#include <thumbcache.h>
#include <shlwapi.h>
#include <stdio.h>

static void usage() {
    fputs(
        "abthumb " ABT_VERSION_STR "\n"
        "usage:\n"
        "  abthumb register [--isolated] [--dll PATH]   per-user registration\n"
        "  abthumb unregister\n"
        "  abthumb status                               show handler registration per extension\n"
        "  abthumb shelltest PATH... [--size N] [--force] [--repeat N] [--out DIR]\n"
        "        drive the real shell thumbnail pipeline (IShellItemImageFactory, or\n"
        "        IThumbnailCache with WTS_FORCEEXTRACTION when --force)\n"
        "  abthumb render PATH... [--size N | --box W H] [--page N] [--fast|--render|--probe] [--max]\n"
        "        [--repeat N] [--out DIR]   run the render pipeline in-process (no agent)\n"
        "  abthumb previewtest FILE [--out PNG] [--inproc] [--size W H] [--wait MS]\n"
        "        host the preview handler like Explorer's preview pane and capture it\n",
        stderr);
}

static bool arg_is(const wchar_t* a, const wchar_t* b) { return lstrcmpiW(a, b) == 0; }

static int cmd_register(int argc, wchar_t** argv) {
    abt::RegisterOptions opt = {true};
    wchar_t dll[MAX_PATH] = L"";
    for (int i = 0; i < argc; ++i) {
        if (arg_is(argv[i], L"--isolated")) opt.in_process = false;
        else if (arg_is(argv[i], L"--dll") && i + 1 < argc) GetFullPathNameW(argv[++i], MAX_PATH, dll, nullptr);
    }
    if (!dll[0]) {
        wchar_t dir[MAX_PATH];
        abt::module_dir(nullptr, dir, MAX_PATH);
        _snwprintf_s(dll, MAX_PATH, _TRUNCATE, L"%s\\abthumb.dll", dir);
    }
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "DLL not found: %ls\n", dll);
        return 2;
    }
    HRESULT hr = abt::shell_register(dll, opt);
    printf("register %ls (%s): 0x%08lx\n", dll, opt.in_process ? "in-process" : "isolated", hr);
    return SUCCEEDED(hr) ? 0 : 1;
}

static int cmd_status() {
    char buf[4096];
    abt::shell_describe(buf, sizeof(buf));
    fputs(buf, stdout);
    printf("in-process: %lu\n", abt::reg_get_dword(HKEY_CURRENT_USER, ABT_REG_ROOT, L"InProcess", 0));
    return 0;
}

struct ShellTestOpts {
    int size = 256;
    int repeat = 1;
    bool force = false;
    bool incache = false;  // IThumbnailCache WTS_INCACHEONLY: is it in Explorer's cache?
    const wchar_t* out = nullptr;
};

static HRESULT shell_thumb(const wchar_t* path, const ShellTestOpts& o, HBITMAP* out, DWORD* flags_out) {
    *out = nullptr;
    *flags_out = 0;
    IShellItem* item = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(path, nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr)) return hr;
    if (o.force) {
        IThumbnailCache* tc = nullptr;
        hr = CoCreateInstance(CLSID_LocalThumbnailCache, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tc));
        if (SUCCEEDED(hr)) {
            ISharedBitmap* sb = nullptr;
            WTS_CACHEFLAGS cf = WTS_DEFAULT;
            hr = tc->GetThumbnail(item, (UINT)o.size, o.incache ? WTS_INCACHEONLY : WTS_FORCEEXTRACTION, &sb, &cf, nullptr);
            if (SUCCEEDED(hr) && sb) {
                HBITMAP shared = nullptr;
                hr = sb->Detach(&shared);
                *out = shared;
                sb->Release();
            }
            *flags_out = (DWORD)cf;
            tc->Release();
        }
    } else {
        IShellItemImageFactory* f = nullptr;
        hr = item->QueryInterface(IID_PPV_ARGS(&f));
        if (SUCCEEDED(hr)) {
            SIZE sz = {o.size, o.size};
            hr = f->GetImage(sz, SIIGBF_THUMBNAILONLY, out);
            f->Release();
        }
    }
    item->Release();
    return hr;
}

static int cmd_shelltest(int argc, wchar_t** argv) {
    ShellTestOpts o;
    int first_path = -1, npaths = 0;
    const wchar_t* paths[256];
    for (int i = 0; i < argc; ++i) {
        if (arg_is(argv[i], L"--size") && i + 1 < argc) o.size = _wtoi(argv[++i]);
        else if (arg_is(argv[i], L"--repeat") && i + 1 < argc) o.repeat = _wtoi(argv[++i]);
        else if (arg_is(argv[i], L"--force")) o.force = true;
        else if (arg_is(argv[i], L"--incache")) { o.force = true; o.incache = true; }
        else if (arg_is(argv[i], L"--out") && i + 1 < argc) o.out = argv[++i];
        else if (npaths < 256) paths[npaths++] = argv[i];
    }
    (void)first_path;
    if (!npaths) { usage(); return 2; }
    int failures = 0;
    for (int p = 0; p < npaths; ++p) {
        wchar_t full[MAX_PATH];
        GetFullPathNameW(paths[p], MAX_PATH, full, nullptr);
        double best = 1e30, total = 0;
        HRESULT hr = S_OK;
        HBITMAP bmp = nullptr;
        DWORD flags = 0;
        for (int r = 0; r < o.repeat; ++r) {
            if (bmp) { DeleteObject(bmp); bmp = nullptr; }
            uint64_t t0 = abt::qpc();
            hr = shell_thumb(full, o, &bmp, &flags);
            double ms = abt::qpc_ms(t0, abt::qpc());
            total += ms;
            if (ms < best) best = ms;
            if (FAILED(hr)) break;
        }
        int w = 0, h = 0;
        if (SUCCEEDED(hr) && bmp) {
            BITMAP bm = {};
            GetObjectW(bmp, sizeof(bm), &bm);
            w = bm.bmWidth;
            h = bm.bmHeight;
            if (o.out) {
                uint8_t* px = nullptr;
                int bw, bh;
                if (SUCCEEDED(abt::hbitmap_to_bgra(bmp, &px, &bw, &bh))) {
                    wchar_t png[MAX_PATH];
                    _snwprintf_s(png, MAX_PATH, _TRUNCATE, L"%s\\%s_%d.png", o.out, PathFindFileNameW(full), o.size);
                    abt::save_png_bgra(png, px, bw, bh, bw * 4, true);
                    abt::mem_free(px);
                }
            }
        }
        if (bmp) DeleteObject(bmp);
        if (FAILED(hr)) ++failures;
        printf("%-60ls hr=0x%08lx %4dx%-4d flags=0x%lx best=%.2f ms avg=%.2f ms\n", PathFindFileNameW(full), hr, w, h,
               flags, best, total / o.repeat);
    }
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- render (in-process pipeline)

static const char* source_str(uint32_t s) {
    if (s & ABT_SRC_CACHE) return "cache";
    if (s & ABT_SRC_RENDERED) return "render";
    if (s & ABT_SRC_EMBEDDED) return (s & ABT_SRC_SCALED) ? "embedded(small)" : "embedded";
    return "-";
}

static bool is_supported(const wchar_t* path) {
    const wchar_t* e = abt::path_ext(path);
    for (int i = 0; i < abt::kExtensionCount; ++i)
        if (!lstrcmpiW(e, abt::kExtensions[i])) return true;
    return false;
}

struct RenderOpts {
    int cx = 256, cy = 0, page = 0, repeat = 1;
    uint32_t mode = ABT_MODE_AUTO, flags = 0;
    const wchar_t* out = nullptr;
};

static int render_one(const wchar_t* path, const RenderOpts& o, double* total_ms) {
    abt::RenderRequest rq = {};
    rq.path = path;
    rq.cx = o.cx;
    rq.cy = o.cy ? o.cy : o.cx;
    rq.page = o.page;
    rq.mode = o.mode;
    rq.flags = o.flags;
    abt::RenderResult rr = {};
    double best = 1e30;
    for (int r = 0; r < o.repeat; ++r) {
        if (r) abt::render_result_free(&rr);
        abt::render_file(rq, &rr);
        double ms = rr.elapsed_us / 1000.0;
        if (ms < best) best = ms;
    }
    *total_ms += best;
    printf("%-44ls %-6s %-16s %4dx%-4d %8.2f ms  pages=%u  %s  [%s]\n", PathFindFileNameW(path),
           abt_family_str((abt_family)rr.info.family), source_str(rr.source), rr.img.w, rr.img.h, best,
           rr.info.page_count, rr.info.version, rr.reason);
    if (SUCCEEDED(rr.hr) && o.out && rr.img.px) {
        wchar_t png[MAX_PATH];
        _snwprintf_s(png, MAX_PATH, _TRUNCATE, L"%s\\%s_%d.png", o.out, PathFindFileNameW(path), o.cx);
        abt::save_png_bgra(png, rr.img.px, rr.img.w, rr.img.h, rr.img.w * 4, true);
    }
    int rc = SUCCEEDED(rr.hr) ? 0 : 1;
    abt::render_result_free(&rr);
    return rc;
}

static int cmd_render(int argc, wchar_t** argv) {
    RenderOpts o;
    const wchar_t* paths[512];
    int np = 0;
    for (int i = 0; i < argc; ++i) {
        if (arg_is(argv[i], L"--size") && i + 1 < argc) o.cx = _wtoi(argv[++i]);
        else if (arg_is(argv[i], L"--box") && i + 2 < argc) { o.cx = _wtoi(argv[++i]); o.cy = _wtoi(argv[++i]); }
        else if (arg_is(argv[i], L"--page") && i + 1 < argc) o.page = _wtoi(argv[++i]);
        else if (arg_is(argv[i], L"--repeat") && i + 1 < argc) o.repeat = _wtoi(argv[++i]);
        else if (arg_is(argv[i], L"--out") && i + 1 < argc) o.out = argv[++i];
        else if (arg_is(argv[i], L"--fast")) o.mode = ABT_MODE_FAST;
        else if (arg_is(argv[i], L"--render")) o.mode = ABT_MODE_RENDER;
        else if (arg_is(argv[i], L"--probe")) o.mode = ABT_MODE_PROBE;
        else if (arg_is(argv[i], L"--max")) o.flags |= ABT_RF_MAX_QUALITY;
        else if (np < 512) paths[np++] = argv[i];
    }
    if (!np) { usage(); return 2; }
    if (o.out) CreateDirectoryW(o.out, nullptr);
    abt::render_init(false);
    int fails = 0, count = 0;
    double total = 0;
    for (int p = 0; p < np; ++p) {
        wchar_t full[MAX_PATH];
        GetFullPathNameW(paths[p], MAX_PATH, full, nullptr);
        DWORD attr = GetFileAttributesW(full);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            wchar_t pat[MAX_PATH];
            _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%s\\*", full);
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                wchar_t f[MAX_PATH];
                _snwprintf_s(f, MAX_PATH, _TRUNCATE, L"%s\\%s", full, fd.cFileName);
                if (!is_supported(f)) continue;
                fails += render_one(f, o, &total);
                ++count;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        } else {
            fails += render_one(full, o, &total);
            ++count;
        }
    }
    printf("%d files, %d failed, total %.1f ms, mean %.2f ms\n", count, fails, total, count ? total / count : 0.0);
    abt::render_shutdown();
    return fails ? 1 : 0;
}

static LRESULT CALLBACK host_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

// Hosts our preview handler like Explorer's preview pane does (out of process via the
// registered surrogate unless --inproc), captures the window into a PNG.
static int cmd_previewtest(int argc, wchar_t** argv) {
    const wchar_t* file = nullptr;
    const wchar_t* out = nullptr;
    bool inproc = false;
    int w = 800, h = 600, wait_ms = 1500;
    for (int i = 0; i < argc; ++i) {
        if (arg_is(argv[i], L"--out") && i + 1 < argc) out = argv[++i];
        else if (arg_is(argv[i], L"--inproc")) inproc = true;
        else if (arg_is(argv[i], L"--size") && i + 2 < argc) { w = _wtoi(argv[++i]); h = _wtoi(argv[++i]); }
        else if (arg_is(argv[i], L"--wait") && i + 1 < argc) wait_ms = _wtoi(argv[++i]);
        else file = argv[i];
    }
    if (!file) { usage(); return 2; }
    wchar_t full[MAX_PATH];
    GetFullPathNameW(file, MAX_PATH, full, nullptr);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = host_wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"abthumbPreviewHost";
    wc.hbrBackground = (HBRUSH)GetStockObject(GRAY_BRUSH);
    RegisterClassW(&wc);
    RECT wr = {0, 0, w, h};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    HWND host = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"abthumb preview test", WS_OVERLAPPEDWINDOW,
                                0, 0, wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(host, SW_SHOWNOACTIVATE);

    uint64_t t0 = abt::qpc();
    IPreviewHandler* ph = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_AbtPreviewHandler, nullptr, inproc ? CLSCTX_INPROC_SERVER : CLSCTX_LOCAL_SERVER,
                                  IID_PPV_ARGS(&ph));
    printf("CoCreateInstance(%s): 0x%08lx  %.1f ms\n", inproc ? "inproc" : "local server", hr, abt::qpc_ms(t0, abt::qpc()));
    if (FAILED(hr)) { DestroyWindow(host); return 1; }
    IInitializeWithItem* iwi = nullptr;
    IInitializeWithFile* iwf = nullptr;
    if (SUCCEEDED(ph->QueryInterface(IID_PPV_ARGS(&iwi)))) {
        IShellItem* item = nullptr;
        hr = SHCreateItemFromParsingName(full, nullptr, IID_PPV_ARGS(&item));
        if (SUCCEEDED(hr)) { hr = iwi->Initialize(item, STGM_READ); item->Release(); }
        iwi->Release();
        printf("Initialize(item): 0x%08lx\n", hr);
    } else if (SUCCEEDED(ph->QueryInterface(IID_PPV_ARGS(&iwf)))) {
        hr = iwf->Initialize(full, STGM_READ);
        iwf->Release();
        printf("Initialize(file): 0x%08lx\n", hr);
    }
    RECT rc = {0, 0, w, h};
    if (SUCCEEDED(hr)) hr = ph->SetWindow(host, &rc);
    if (SUCCEEDED(hr)) hr = ph->DoPreview();
    printf("DoPreview: 0x%08lx  %.1f ms since create\n", hr, abt::qpc_ms(t0, abt::qpc()));
    uint64_t until = abt::qpc();
    while (abt::qpc_ms(until, abt::qpc()) < wait_ms) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(10);
    }
    if (out) {
        void* bits = nullptr;
        HBITMAP bmp = abt::create_dib32(w, h, &bits);
        HDC wdc = GetDC(host);
        HDC mdc = CreateCompatibleDC(wdc);
        HGDIOBJ old = SelectObject(mdc, bmp);
        // In-process: ask our window to paint itself (works off-screen). Out of process: DWM capture.
        IOleWindow* ow = nullptr;
        HWND child = nullptr;
        if (inproc && SUCCEEDED(ph->QueryInterface(IID_PPV_ARGS(&ow)))) { ow->GetWindow(&child); ow->Release(); }
        if (child) SendMessageW(child, WM_PRINTCLIENT, (WPARAM)mdc, PRF_CLIENT);
        else PrintWindow(host, mdc, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
        SelectObject(mdc, old);
        DeleteDC(mdc);
        ReleaseDC(host, wdc);
        for (int i = 0; i < w * h; ++i) ((uint8_t*)bits)[i * 4 + 3] = 255;
        HRESULT shr = abt::save_png_bgra(out, (const uint8_t*)bits, w, h, w * 4, false);
        printf("capture -> %ls: 0x%08lx\n", out, shr);
        DeleteObject(bmp);
    }
    ph->Unload();
    ph->Release();
    DestroyWindow(host);
    return SUCCEEDED(hr) ? 0 : 1;
}

int wmain(int argc, wchar_t** argv) {
    abt::log_init(L"cli");
    if (argc < 2) { usage(); return 2; }
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) return 3;
    int rc = 2;
    const wchar_t* cmd = argv[1];
    if (arg_is(cmd, L"register")) rc = cmd_register(argc - 2, argv + 2);
    else if (arg_is(cmd, L"unregister")) { hr = abt::shell_unregister(); printf("unregister: 0x%08lx\n", hr); rc = 0; }
    else if (arg_is(cmd, L"status")) rc = cmd_status();
    else if (arg_is(cmd, L"shelltest")) rc = cmd_shelltest(argc - 2, argv + 2);
    else if (arg_is(cmd, L"previewtest")) rc = cmd_previewtest(argc - 2, argv + 2);
    else if (arg_is(cmd, L"render")) rc = cmd_render(argc - 2, argv + 2);
    else if (arg_is(cmd, L"stats") || arg_is(cmd, L"stop-agent") || arg_is(cmd, L"ping")) {
        abt_req req;
        abt::init_req(&req, arg_is(cmd, L"stats") ? ABT_MSG_STATS : arg_is(cmd, L"ping") ? ABT_MSG_PING : ABT_MSG_QUIT, 0);
        abt_resp resp;
        uint8_t* px = nullptr;
        uint64_t t0 = abt::qpc();
        hr = abt::pipe_call(req, L"", 5000, &resp, &px);
        if (FAILED(hr)) printf("agent not reachable: 0x%08lx\n", hr);
        else if (px) printf("%.*s", (int)resp.pixel_len, (const char*)px);
        else printf("ok (%.2f ms)\n", abt::qpc_ms(t0, abt::qpc()));
        abt::mem_free(px);
        rc = SUCCEEDED(hr) ? 0 : 1;
    }
    else if (arg_is(cmd, L"comtest") && argc > 2) {
        CLSID clsid;
        hr = CLSIDFromString(argv[2], &clsid);
        IUnknown* unk = nullptr;
        uint64_t t0 = abt::qpc();
        if (SUCCEEDED(hr)) hr = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_IUnknown, (void**)&unk);
        printf("CoCreateInstance(LOCAL_SERVER, %ls): 0x%08lx  %.1f ms\n", argv[2], hr, abt::qpc_ms(t0, abt::qpc()));
        if (unk) unk->Release();
        rc = SUCCEEDED(hr) ? 0 : 1;
    }
    else usage();
    CoUninitialize();
    return rc;
}
