// abthumb.exe: registration, shell tests, (later) install, render, bench, stats.
#include "abt_win.h"
#include "abt_registry.h"
#include "abt_wic.h"
#include "abt_proto.h"
#include "render.h"
#include "abt_decode.h"
#include <shobjidl.h>
#include <thumbcache.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <shlguid.h>
#include <shellapi.h>
#include <stdio.h>

static void usage() {
    fputs(
        "abthumb " ABT_VERSION_STR "\n"
        "usage:\n"
        "  (install/uninstall: run abthumb-setup.exe / Settings > Apps)\n"
        "  abthumb stats | ping | stop-agent             talk to the running agent\n"
        "  (developer registration: regsvr32 abthumb.dll  /  regsvr32 /u abthumb.dll)\n"
        "  abthumb status                               show handler registration per extension\n"
        "  abthumb shelltest PATH... [--size N] [--force] [--repeat N] [--out DIR]\n"
        "        drive the real shell thumbnail pipeline (IShellItemImageFactory, or\n"
        "        IThumbnailCache with WTS_FORCEEXTRACTION when --force)\n"
        "  abthumb render PATH... [--size N | --box W H] [--page N | --canvas] [--fast|--render|--probe] [--max]\n"
        "        [--repeat N] [--out DIR]   run the render pipeline in-process (no agent)\n"
        "  abthumb bench DIR... [--size N] [--agent] [-v]   latency percentiles vs the WIC/FPV path\n"
        "  abthumb thumbtest FILE [--size N] [--repeat N]   our provider in-process (DLL -> agent)\n"
        "  abthumb previewtest FILE [--out PNG] [--inproc | --dll PATH] [--size W H] [--wait MS] [--nav N [--step MS]]\n"
        "        host the preview handler like Explorer's preview pane and capture it\n",
        stderr);
}

static bool arg_is(const wchar_t* a, const wchar_t* b) { return lstrcmpiW(a, b) == 0; }


// Registration is deliberately not in this tool: use `regsvr32 abthumb.dll` (machine-wide when
// elevated, per-user developer mode otherwise) or the setup package. Unsigned tools that write COM class
// registrations get quarantined by antivirus heuristics (D-021).

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
            DIBSECTION ds = {};
            int dsz = GetObjectW(bmp, sizeof(ds), &ds);
            printf("  [bitmap: %s, biHeight=%ld, bpp=%u]\n", dsz == sizeof(ds) ? "DIB section" : "DDB",
                   dsz == sizeof(ds) ? ds.dsBmih.biHeight : 0L, bm.bmBitsPixel);
            if (o.out) {
                uint8_t* px = nullptr;
                int bw, bh;
                if (SUCCEEDED(abt::hbitmap_to_bgra(bmp, &px, &bw, &bh, true))) {
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
        else if (arg_is(argv[i], L"--canvas")) o.page = (int)ABT_PAGE_CANVAS;  // what thumbnails show
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

// ---------------------------------------------------------------- bench
// Per file: our pipeline (auto / forced render / embedded only), the WIC path that the
// FastPictureViewer codecs plug into (baseline), and the agent (cache miss vs cache hit).

static double wic_thumb_ms(const wchar_t* path, int size, bool* ok) {
    *ok = false;
    IWICImagingFactory* f = nullptr;
    if (FAILED(abt::wic_factory(&f))) return 0;
    uint64_t t0 = abt::qpc();
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICBitmapSource* thumb = nullptr;
    IWICBitmapScaler* sc = nullptr;
    HRESULT hr = f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &fr);
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) {
        // The photo thumbnail provider asks the codec for its thumbnail first, then the frame.
        if (FAILED(fr->GetThumbnail(&thumb)) && FAILED(dec->GetThumbnail(&thumb))) thumb = nullptr;
        IWICBitmapSource* src = thumb ? thumb : (IWICBitmapSource*)fr;
        hr = src->GetSize(&w, &h);
        int dw, dh;
        abt_fit((int)w, (int)h, size, 0, &dw, &dh);
        if (SUCCEEDED(hr)) hr = f->CreateBitmapScaler(&sc);
        if (SUCCEEDED(hr)) hr = sc->Initialize(src, (UINT)dw, (UINT)dh, WICBitmapInterpolationModeFant);
        if (SUCCEEDED(hr)) {
            IWICFormatConverter* cv = nullptr;
            hr = f->CreateFormatConverter(&cv);
            if (SUCCEEDED(hr)) hr = cv->Initialize(sc, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
            uint8_t* px = SUCCEEDED(hr) ? (uint8_t*)malloc((size_t)dw * dh * 4) : nullptr;
            if (px) hr = cv->CopyPixels(nullptr, (UINT)dw * 4, (UINT)(dw * dh * 4), px);
            free(px);
            if (cv) cv->Release();
        }
    }
    double ms = abt::qpc_ms(t0, abt::qpc());
    if (sc) sc->Release();
    if (thumb) thumb->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    f->Release();
    *ok = SUCCEEDED(hr);
    return ms;
}

struct Series {
    double v[4096];
    int n = 0;
    void add(double x) { if (n < 4096) v[n++] = x; }
};
static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}
static void summary(const char* name, Series& s) {
    if (!s.n) { printf("  %-22s  (no samples)\n", name); return; }
    qsort(s.v, (size_t)s.n, sizeof(double), cmp_d);
    double sum = 0;
    for (int i = 0; i < s.n; ++i) sum += s.v[i];
#pragma warning(suppress : 6385)  // /analyze false positive: s.n >= 1 here, so s.n - 1 is a valid index
    printf("  %-22s n=%-4d mean %8.2f  p50 %8.2f  p95 %8.2f  max %8.2f ms\n", name, s.n, sum / s.n, s.v[s.n / 2],
           s.v[(int)(s.n * 0.95) < s.n ? (int)(s.n * 0.95) : s.n - 1], s.v[s.n - 1]);
}

static double pipe_thumb_ms(const wchar_t* path, int size, uint32_t flags, HRESULT* hr_out, uint32_t* source) {
    abt_req req;
    abt::init_req(&req, ABT_MSG_THUMB, (uint32_t)lstrlenW(path));
    req.cx = req.cy = (uint32_t)size;
    req.flags = flags;
    abt_resp resp;
    uint8_t* px = nullptr;
    uint64_t t0 = abt::qpc();
    *hr_out = abt::pipe_call(req, path, 60000, &resp, &px);
    double ms = abt::qpc_ms(t0, abt::qpc());
    *source = resp.source;
    abt::mem_free(px);
    return ms;
}

static int cmd_bench(int argc, wchar_t** argv) {
    int size = 256;
    bool agent = false, verbose = false;
    const wchar_t* list = nullptr;
    const wchar_t* dirs[64];
    int nd = 0;
    for (int i = 0; i < argc; ++i) {
        if (arg_is(argv[i], L"--size") && i + 1 < argc) size = _wtoi(argv[++i]);
        else if (arg_is(argv[i], L"--agent")) agent = true;
        else if (arg_is(argv[i], L"-v")) verbose = true;
        else if (arg_is(argv[i], L"--list") && i + 1 < argc) list = argv[++i];
        else if (nd < 64) dirs[nd++] = argv[i];
    }
    if (!nd && !list) { usage(); return 2; }
    abt::render_init(false);
    static Series s_auto, s_render, s_fast, s_wic, s_miss, s_hit;
    int files = 0, wic_fail = 0, auto_fail = 0;
    // Collect paths: directory listings, then an optional UTF-8/UTF-16 list file (one path per line).
    static wchar_t paths[4096][MAX_PATH];
    int np = 0;
    for (int d = 0; d < nd; ++d) {
        wchar_t pat[MAX_PATH], dir[MAX_PATH];
        GetFullPathNameW(dirs[d], MAX_PATH, dir, nullptr);
        _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%s\\*", dir);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || np >= 4096) continue;
            _snwprintf_s(paths[np], MAX_PATH, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);
            if (is_supported(paths[np])) ++np;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (list) {
        FILE* lf = nullptr;
        if (_wfopen_s(&lf, list, L"rt, ccs=UTF-8") == 0 && lf) {
            wchar_t line[MAX_PATH];
            while (np < 4096 && fgetws(line, MAX_PATH, lf)) {
                size_t l = wcslen(line);
                while (l && (line[l - 1] == L'\n' || line[l - 1] == L'\r')) line[--l] = 0;
                if (l && is_supported(line)) lstrcpynW(paths[np++], line, MAX_PATH);
            }
            fclose(lf);
        }
    }
    for (int pi = 0; pi < np; ++pi) {
        {
            const wchar_t* f = paths[pi];
            struct { const wchar_t* cFileName; } fd = {PathFindFileNameW(f)};
            ++files;
            abt::RenderRequest rq = {};
            rq.path = f;
            rq.cx = rq.cy = size;
            double best[3] = {1e30, 1e30, 1e30};
            uint32_t modes[3] = {ABT_MODE_AUTO, ABT_MODE_RENDER, ABT_MODE_FAST};
            bool okm[3] = {};
            for (int m = 0; m < 3; ++m) {
                rq.mode = modes[m];
                for (int r = 0; r < 3; ++r) {
                    abt::RenderResult rr;
                    abt::render_file(rq, &rr);
                    okm[m] = SUCCEEDED(rr.hr);
                    double ms = rr.elapsed_us / 1000.0;
                    if (ms < best[m]) best[m] = ms;
                    abt::render_result_free(&rr);
                    if (!okm[m]) break;
                }
            }
            if (okm[0]) s_auto.add(best[0]); else ++auto_fail;
            if (okm[1]) s_render.add(best[1]);
            if (okm[2]) s_fast.add(best[2]);
            bool wok = false;
            double wbest = 1e30;
            for (int r = 0; r < 3; ++r) {
                double ms = wic_thumb_ms(f, size, &wok);
                if (!wok) break;
                if (ms < wbest) wbest = ms;
            }
            if (wok) s_wic.add(wbest); else ++wic_fail;
            double miss = 0, hit = 0;
            if (agent) {
                HRESULT hr;
                uint32_t src;
                miss = pipe_thumb_ms(f, size, ABT_RF_NO_CACHE, &hr, &src);
                if (SUCCEEDED(hr)) s_miss.add(miss);
                pipe_thumb_ms(f, size, 0, &hr, &src);  // make sure it is cached
                hit = pipe_thumb_ms(f, size, 0, &hr, &src);
                if (SUCCEEDED(hr) && (src & ABT_SRC_CACHE)) s_hit.add(hit);
            }
            if (verbose)
                printf("%-44ls auto %7.2f render %7.2f fast %7.2f wic %7.2f%s agent miss %7.2f hit %6.2f\n",
                       fd.cFileName, okm[0] ? best[0] : -1, okm[1] ? best[1] : -1, okm[2] ? best[2] : -1,
                       wok ? wbest : -1, wok ? "" : "(no codec)", miss, hit);
        }
    }
    printf("%d files at %d px (best of 3 per file; render worker path in-process, warm OS file cache)\n", files, size);
    summary("abthumb auto", s_auto);
    summary("abthumb forced render", s_render);
    summary("abthumb embedded only", s_fast);
    summary("WIC/FPV codec path", s_wic);
    if (agent) {
        summary("agent, cache miss", s_miss);
        summary("agent, cache hit", s_hit);
    }
    printf("  failures: abthumb auto %d, WIC/FPV %d (no codec or decode error)\n", auto_fail, wic_fail);
    abt::render_shutdown();
    return 0;
}

static LRESULT CALLBACK host_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

// Hosts our preview handler like Explorer's preview pane does (out of process via the
// registered surrogate unless --inproc), captures the window into a PNG.
static int cmd_previewtest(int argc, wchar_t** argv) {
    const wchar_t* file = nullptr;
    const wchar_t* out = nullptr;
    const wchar_t* dll = nullptr;  // load this build's DLL directly (no registration needed)
    bool inproc = false;
    int w = 800, h = 600, wait_ms = 1500, nav = 0, step_ms = 400;
    for (int i = 0; i < argc; ++i) {
        if (arg_is(argv[i], L"--out") && i + 1 < argc) out = argv[++i];
        else if (arg_is(argv[i], L"--inproc")) inproc = true;
        else if (arg_is(argv[i], L"--size") && i + 2 < argc) { w = _wtoi(argv[++i]); h = _wtoi(argv[++i]); }
        else if (arg_is(argv[i], L"--wait") && i + 1 < argc) wait_ms = _wtoi(argv[++i]);
        else if (arg_is(argv[i], L"--dll") && i + 1 < argc) { dll = argv[++i]; inproc = true; }
        else if (arg_is(argv[i], L"--nav") && i + 1 < argc) nav = _wtoi(argv[++i]);  // press Right N times
        else if (arg_is(argv[i], L"--step") && i + 1 < argc) step_ms = _wtoi(argv[++i]);
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
    HRESULT hr = E_FAIL;
    if (dll) {
        HMODULE mod = LoadLibraryW(dll);
        typedef HRESULT(STDAPICALLTYPE * GetClassObjectFn)(REFCLSID, REFIID, void**);
        GetClassObjectFn gco = mod ? (GetClassObjectFn)GetProcAddress(mod, "DllGetClassObject") : nullptr;
        IClassFactory* cf = nullptr;
        hr = gco ? gco(CLSID_AbtPreviewHandler, IID_PPV_ARGS(&cf)) : HRESULT_FROM_WIN32(GetLastError());
        if (SUCCEEDED(hr)) { hr = cf->CreateInstance(nullptr, IID_PPV_ARGS(&ph)); cf->Release(); }
    } else {
        hr = CoCreateInstance(CLSID_AbtPreviewHandler, nullptr, inproc ? CLSCTX_INPROC_SERVER : CLSCTX_LOCAL_SERVER,
                              IID_PPV_ARGS(&ph));
    }
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
    IOleWindow* ow = nullptr;
    HWND child = nullptr;
    if (inproc && SUCCEEDED(ph->QueryInterface(IID_PPV_ARGS(&ow)))) { ow->GetWindow(&child); ow->Release(); }
    // --nav N: after the first wait, press Right N times (step_ms apart), capturing after each step
    // into <out>.<step>.png, to check artboard cycling.
    for (int step = 0; step <= nav; ++step) {
        if (step && child) {
            SendMessageW(child, WM_KEYDOWN, VK_RIGHT, 0);
            printf("step %d: Right\n", step);
        }
        uint64_t until = abt::qpc();
        int ms = step ? step_ms : wait_ms;
        while (abt::qpc_ms(until, abt::qpc()) < ms) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            Sleep(5);
        }
        if (!out) continue;
        wchar_t path[MAX_PATH];
        if (step) _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s.%d.png", out, step);
        else lstrcpynW(path, out, MAX_PATH);
        void* bits = nullptr;
        HBITMAP bmp = abt::create_dib32(w, h, &bits);
        HDC wdc = GetDC(host);
        HDC mdc = CreateCompatibleDC(wdc);
        HGDIOBJ old = SelectObject(mdc, bmp);
        // In-process: ask our window to paint itself (works off-screen). Out of process: DWM capture.
        if (child) SendMessageW(child, WM_PRINTCLIENT, (WPARAM)mdc, PRF_CLIENT);
        else PrintWindow(host, mdc, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
        SelectObject(mdc, old);
        DeleteDC(mdc);
        ReleaseDC(host, wdc);
        for (int i = 0; i < w * h; ++i) ((uint8_t*)bits)[i * 4 + 3] = 255;
        HRESULT shr = abt::save_png_bgra(path, (const uint8_t*)bits, w, h, w * 4, false);
        printf("capture -> %ls: 0x%08lx\n", path, shr);
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
    if (arg_is(cmd, L"status")) rc = cmd_status();
    else if (arg_is(cmd, L"shelltest")) rc = cmd_shelltest(argc - 2, argv + 2);
    else if (arg_is(cmd, L"previewtest")) rc = cmd_previewtest(argc - 2, argv + 2);
    else if (arg_is(cmd, L"render")) rc = cmd_render(argc - 2, argv + 2);
    else if (arg_is(cmd, L"bench")) rc = cmd_bench(argc - 2, argv + 2);
    else if (arg_is(cmd, L"thumbtest") && argc > 2) {
        // Our thumbnail provider created directly in-process (DLL -> agent -> worker), independent
        // of which handler the shell would pick (useful before a machine-wide install).
        int size = 256, repeat = 1;
        for (int i = 3; i + 1 < argc; ++i) {
            if (arg_is(argv[i], L"--size")) size = _wtoi(argv[i + 1]);
            if (arg_is(argv[i], L"--repeat")) repeat = _wtoi(argv[i + 1]);
        }
        wchar_t full[MAX_PATH];
        GetFullPathNameW(argv[2], MAX_PATH, full, nullptr);
        rc = 1;
        for (int r = 0; r < repeat; ++r) {
            uint64_t t0 = abt::qpc();
            IThumbnailProvider* tp = nullptr;
            IInitializeWithItem* iwi = nullptr;
            IShellItem* item = nullptr;
            HBITMAP bmp = nullptr;
            WTS_ALPHATYPE at = WTSAT_UNKNOWN;
            hr = CoCreateInstance(CLSID_AbtThumbnailProvider, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tp));
            if (SUCCEEDED(hr)) hr = tp->QueryInterface(IID_PPV_ARGS(&iwi));
            if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(full, nullptr, IID_PPV_ARGS(&item));
            if (SUCCEEDED(hr)) hr = iwi->Initialize(item, STGM_READ);
            if (SUCCEEDED(hr)) hr = tp->GetThumbnail((UINT)size, &bmp, &at);
            BITMAP bm = {};
            if (bmp) GetObjectW(bmp, sizeof(bm), &bm);
            printf("%-44ls hr=0x%08lx %4ldx%-4ld alpha=%d %.2f ms\n", PathFindFileNameW(full), hr, bm.bmWidth,
                   bm.bmHeight, (int)at, abt::qpc_ms(t0, abt::qpc()));
            if (bmp) DeleteObject(bmp);
            if (item) item->Release();
            if (iwi) iwi->Release();
            if (tp) tp->Release();
            if (SUCCEEDED(hr)) rc = 0;
        }
    }
    else if (arg_is(cmd, L"cmyktest")) {  // prints sRGB for pure C, M, Y, K, white through the ICM transform
        abt::CmykXform* x = abt::cmyk_xform_create(nullptr, 0);
        const uint8_t in[5][4] = {{255, 0, 0, 0}, {0, 255, 0, 0}, {0, 0, 255, 0}, {0, 0, 0, 255}, {0, 0, 0, 0}};
        const char* names[5] = {"C", "M", "Y", "K", "white"};
        for (int i = 0; i < 5; ++i) {
            uint8_t rgb[3] = {};
            int ok = x ? abt::cmyk_xform_fn(x, in[i], rgb, 1) : 0;
            printf("%-5s -> ok=%d R=%3u G=%3u B=%3u\n", names[i], ok, rgb[0], rgb[1], rgb[2]);
        }
        abt::cmyk_xform_destroy(x);
        rc = 0;
    }
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
    else usage();
    CoUninitialize();
    return rc;
}
