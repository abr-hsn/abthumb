// Named-pipe server: answers the shell extension (thumbnails, preview pane) and the CLI.
#include "agent.h"
#include <sddl.h>

namespace abt {

Stats g_stats;

static HANDLE g_threads[32];
static int g_nthreads = 0;
static volatile bool g_stop = false;
static PSECURITY_DESCRIPTOR g_sd = nullptr;
static HANDLE g_first = INVALID_HANDLE_VALUE;
static wchar_t g_name[128];

// Current user + SYSTEM; deny network logons; low-integrity label so prevhost.exe (low IL)
// can connect (D-019).
static bool make_sd() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    uint8_t buf[256];
    DWORD len = 0;
    wchar_t* sid = nullptr;
    bool ok = GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &len) &&
              ConvertSidToStringSidW(((TOKEN_USER*)buf)->User.Sid, &sid);
    CloseHandle(tok);
    if (!ok) return false;
    wchar_t sddl[256];
    _snwprintf_s(sddl, _countof(sddl), _TRUNCATE, L"D:P(D;;GA;;;NU)(A;;GA;;;SY)(A;;GA;;;%s)S:(ML;;NW;;;LW)", sid);
    LocalFree(sid);
    return ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &g_sd, nullptr) != 0;
}

static HANDLE create_instance(bool first) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), g_sd, FALSE};
    return CreateNamedPipeW(g_name,
                            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                            PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, g_sd ? &sa : nullptr);
}

static void fit_to_box(abt_image* img, int bw, int bh) {
    if (!img->px || bw <= 0 || bh <= 0) return;
    double s = (double)bw / img->w, s2 = (double)bh / img->h;
    if (s2 < s) s = s2;
    if (s >= 1.0) return;  // never enlarge here
    int dw = (int)(img->w * s + 0.5), dh = (int)(img->h * s + 0.5);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    abt_image r;
    if (abt_resize(img->px, img->w, img->h, (size_t)img->w * 4, dw, dh, &r) == ABT_OK) {
        int alpha = img->has_alpha;
        abt_image_free(img);
        *img = r;
        img->has_alpha = alpha;
    }
}

static uint32_t flags_to_source(uint16_t f) {
    uint32_t s = ABT_SRC_CACHE;
    if (f & CF_EMBEDDED) s |= ABT_SRC_EMBEDDED;
    if (f & CF_RENDERED) s |= ABT_SRC_RENDERED;
    if (f & CF_SCALED) s |= ABT_SRC_SCALED;
    return s;
}

static void serve_image(const abt_req& req, const wchar_t* path, abt_resp* resp, abt_image* img) {
    uint64_t t0 = qpc();
    InterlockedIncrement64(&g_stats.requests);
    FileKey k;
    if (!file_key(path, &k)) {
        resp->hr = HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_FILE_NOT_FOUND);
        lstrcpynA(resp->reason, "cannot open file", sizeof(resp->reason));
        InterlockedIncrement64(&g_stats.failures);
        return;
    }
    int bw = req.cx ? (int)req.cx : 256;
    int bh = (req.msg == ABT_MSG_PREVIEW && req.cy) ? (int)req.cy : bw;
    int edge = bw > bh ? bw : bh;
    bool info_only = req.msg == ABT_MSG_INFO;
    // Thumbnails always show the whole document (Illustrator: the full canvas, D-029); the preview
    // pane asks for one page / artboard at a time.
    const uint32_t page = req.msg == ABT_MSG_PREVIEW ? req.page : ABT_PAGE_CANVAS;
    CacheEntryView view = {};
    if (req.msg == ABT_MSG_PREVIEW && (req.flags & ABT_RF_PREFETCH)) {
        // Neighbouring artboards for the preview pane: start the render and answer at once. The
        // pane's real request later finds it in the cache or joins the job still in flight.
        if (cache_get(k, page, edge, img, &view)) {
            abt_image_free(img);
            resp->hr = S_OK;
            lstrcpynA(resp->reason, "prefetch: cached", sizeof(resp->reason));
        } else if (Job* pj = job_submit(path, k, page, edge, edge, ABT_MODE_AUTO, req.flags & ~ABT_RF_PREFETCH,
                                        LANE_PREVIEW)) {
            job_release(pj);  // the scheduler keeps its own reference; the result goes to the cache
            resp->hr = S_FALSE;
            lstrcpynA(resp->reason, "prefetch: queued", sizeof(resp->reason));
        } else {
            resp->hr = E_OUTOFMEMORY;
        }
        resp->total_len = sizeof(*resp);
        resp->elapsed_us = (uint32_t)(qpc_ms(t0, qpc()) * 1000.0);
        return;
    }
    if (!(req.flags & ABT_RF_NO_CACHE) && !info_only && cache_get(k, page, edge, img, &view)) {
        resp->info = view.info;
        resp->source = flags_to_source(view.flags);
        if (view.flags & CF_NEGATIVE) {
            resp->hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            lstrcpynA(resp->reason, "cached failure (file unchanged since)", sizeof(resp->reason));
            InterlockedIncrement64(&g_stats.negative_hits);
        } else {
            resp->hr = S_OK;
            lstrcpynA(resp->reason, "cache", sizeof(resp->reason));
            InterlockedIncrement64(&g_stats.cache_hits);
        }
    } else {
        InterlockedIncrement64(&g_stats.misses);
        Lane lane = req.msg == ABT_MSG_PREVIEW ? LANE_PREVIEW
                    : (req.flags & ABT_RF_FROM_PREWARM) ? LANE_VISIBLE
                                                         : LANE_FOREGROUND;
        Job* j = job_submit(path, k, page, edge, edge, info_only ? ABT_MODE_PROBE : ABT_MODE_AUTO, req.flags, lane);
        if (!j) {
            resp->hr = E_OUTOFMEMORY;
        } else {
            DWORD wait = req.deadline_ms ? req.deadline_ms : (req.msg == ABT_MSG_PREVIEW ? 25000u : 12000u);
            JobResult r = {};
            bool done = false;
            // Progressive upgrade (D-011): a thumbnail whose render is slow gets the embedded preview
            // now; the render keeps going, lands in our cache, and Explorer's entry is refreshed.
            if (req.msg == ABT_MSG_THUMB && !(req.flags & ABT_RF_FROM_PREWARM) && wait > 350) {
                done = job_wait(j, 350, &r);
                if (!done) {
                    Job* fast = job_submit(path, k, page, edge, edge, ABT_MODE_FAST, req.flags | ABT_RF_NO_CACHE,
                                           LANE_FOREGROUND);
                    JobResult fr = {};  // untouched when job_wait times out
                    if (fast && job_wait(fast, 1500, &fr) && SUCCEEDED(fr.hr) && fr.img.px) {
                        resp->hr = S_OK;
                        resp->info = fr.info;
                        resp->source = fr.source | ABT_SRC_UPGRADE_PENDING;
                        lstrcpynA(resp->reason, "embedded preview; sharper render on its way", sizeof(resp->reason));
                        *img = fr.img;
                        prewarm_upgrade_when_done(j, path, edge);  // takes over our reference
                        j = nullptr;
                    } else if (fast) {
                        abt_image_free(&fr.img);
                    }
                    if (fast) job_release(fast);
                    if (!j) goto finish;
                    wait -= 350;
                }
            }
            if (done || job_wait(j, wait, &r)) {
                resp->hr = r.hr;
                resp->info = r.info;
                resp->source = r.source;
                memcpy(resp->reason, r.reason, sizeof(resp->reason));
                *img = r.img;
                if (FAILED(r.hr)) InterlockedIncrement64(&g_stats.failures);
            } else {
                resp->hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                lstrcpynA(resp->reason, "still rendering (result will be cached)", sizeof(resp->reason));
                InterlockedIncrement64(&g_stats.timeouts);
            }
            job_release(j);
        }
    }
finish:
    if (SUCCEEDED(resp->hr) && img->px) {
        fit_to_box(img, bw, bh);
        resp->width = (uint32_t)img->w;
        resp->height = (uint32_t)img->h;
        resp->pixel_len = resp->width * resp->height * 4;  // <= 16384^2*4 after fit_to_box: no wrap
    } else {
        abt_image_free(img);
    }
    resp->total_len = sizeof(*resp) + resp->pixel_len;
    resp->elapsed_us = (uint32_t)(qpc_ms(t0, qpc()) * 1000.0);
    InterlockedAdd64(&g_stats.total_us, resp->elapsed_us);
    if (req.msg == ABT_MSG_THUMB && !(req.flags & ABT_RF_FROM_PREWARM)) prewarm_hint_file(path, edge);
}

// Who is on the other end. Low-integrity callers (prevhost, sandboxed apps) may only ask for
// images; anything that steers the agent (quit, folder pre-warm, cache bypass) needs medium IL.
struct Caller {
    bool low;   // below medium integrity, or unknown
    bool self;  // our own process (pre-warm calls through IThumbnailCache)
};

static Caller identify_caller(HANDLE p) {
    Caller c = {true, false};
    ULONG pid = 0;
    if (GetNamedPipeClientProcessId(p, &pid)) c.self = pid == GetCurrentProcessId();
    // Clients connect with SECURITY_IDENTIFICATION: enough to read their token, not to act as them.
    if (ImpersonateNamedPipeClient(p)) {
        HANDLE tok = nullptr;
        if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &tok)) {
            c.low = token_below_medium_integrity(tok);
            CloseHandle(tok);
        }
        if (!RevertToSelf()) ExitProcess(1);  // never keep running under a client's identity
    }
    return c;
}

// Paths must be plain drive-letter or UNC paths: no device namespace (\\.\, \\?\GLOBALROOT, \??\),
// no embedded NULs.
static bool acceptable_path(const wchar_t* path, uint32_t chars) {
    if (chars < 3 || wcslen(path) != chars) return false;
    if (!wcsncmp(path, L"\\\\?\\", 4)) {
        path += 4;  // long-path prefix: allow \\?\X:\ and \\?\UNC\ only
        if (!_wcsnicmp(path, L"UNC\\", 4)) return true;
    } else if (!wcsncmp(path, L"\\\\", 2)) {
        return path[2] != L'.' && path[2] != L'?';
    }
    return iswalpha(path[0]) && path[1] == L':' && path[2] == L'\\';
}

static const DWORD kIoTimeoutMs = 5000;

static void handle_client(HANDLE p, wchar_t* path, HANDLE ev) {
    abt_req req;
    if (!read_all_timeout(p, &req, sizeof(req), kIoTimeoutMs, ev) || req.magic != ABT_MAGIC ||
        req.version != ABT_PROTO_VERSION || req.path_chars > ABT_MAX_PATH_CHARS ||
        req.total_len != sizeof(req) + req.path_chars * sizeof(wchar_t))
        return;
    if (req.path_chars && !read_all_timeout(p, path, req.path_chars * sizeof(wchar_t), kIoTimeoutMs, ev)) return;
    path[req.path_chars] = 0;
    const Caller caller = identify_caller(p);
    abt_resp resp;
    init_resp(&resp, (abt_msg)req.msg);
    if (caller.low) {
        req.flags &= ABT_RF_PREFETCH;  // only scheduling hints; the rest steers the agent (D-028)
        if (req.msg != ABT_MSG_THUMB && req.msg != ABT_MSG_PREVIEW && req.msg != ABT_MSG_INFO && req.msg != ABT_MSG_PING)
            req.msg = 0;  // -> E_INVALIDARG below
    }
    if (!caller.self) req.flags &= ~ABT_RF_FROM_PREWARM;
    bool image = req.msg == ABT_MSG_THUMB || req.msg == ABT_MSG_PREVIEW || req.msg == ABT_MSG_INFO;
    if ((image || req.msg == ABT_MSG_HINT) &&
        (!acceptable_path(path, req.path_chars) || req.cx > ABT_MAX_REPLY_DIM || req.cy > ABT_MAX_REPLY_DIM ||
         req.page > 0xFFFF)) {
        resp.hr = E_INVALIDARG;
        write_all_timeout(p, &resp, sizeof(resp), kIoTimeoutMs, ev);
        return;
    }
    abt_image img = {};
    char* text = nullptr;
    size_t text_len = 0;
    switch (req.msg) {
    case ABT_MSG_PING:
        resp.hr = S_OK;
        break;
    case ABT_MSG_QUIT:
        resp.hr = S_OK;
        agent_request_quit();
        break;
    case ABT_MSG_HINT: {
        DWORD a = GetFileAttributesW(path);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) prewarm_hint_folder(path, req.cx ? (int)req.cx : 256);
        else prewarm_hint_file(path, req.cx ? (int)req.cx : 256);
        resp.hr = S_OK;
        break;
    }
    case ABT_MSG_STATS: {
        text = (char*)mem_zalloc(8192);
        if (!text) break;
        int n = _snprintf_s(text, 8192, _TRUNCATE,
                            "requests %lld: cache hits %lld, cached failures %lld, misses %lld, failures %lld, "
                            "timeouts %lld, mean %.2f ms\n",
                            g_stats.requests, g_stats.cache_hits, g_stats.negative_hits, g_stats.misses,
                            g_stats.failures, g_stats.timeouts,
                            g_stats.requests ? g_stats.total_us / 1000.0 / g_stats.requests : 0.0);
        if (n < 0) n = 0;
        cache_stats(text + n, 8192 - n);
        n = (int)strlen(text);
        pool_stats(text + n, 8192 - n);
        n = (int)strlen(text);
        prewarm_stats(text + n, 8192 - n);
        text_len = (strlen(text) + 4) & ~(size_t)3;  // whole "pixels"
        resp.hr = S_OK;
        resp.width = (uint32_t)(text_len / 4);
        resp.height = 1;
        resp.pixel_len = (uint32_t)text_len;
        resp.total_len = sizeof(resp) + resp.pixel_len;
        break;
    }
    case ABT_MSG_THUMB:
    case ABT_MSG_PREVIEW:
    case ABT_MSG_INFO:
        serve_image(req, path, &resp, &img);
        break;
    default:
        resp.hr = E_INVALIDARG;
        break;
    }
    // Bounded writes: a client that stops reading cannot park this thread.
    const DWORD out_timeout = kIoTimeoutMs + resp.pixel_len / 4096;  // ~4 MB/s floor for big previews
    if (write_all_timeout(p, &resp, sizeof(resp), kIoTimeoutMs, ev) && resp.pixel_len)
        write_all_timeout(p, text ? (void*)text : (void*)img.px, resp.pixel_len, out_timeout, ev);
    mem_free(text);
    abt_image_free(&img);
}

// The client closes its end once it has read the whole reply; wait for that (bounded) so that
// DisconnectNamedPipe does not discard unread bytes. Replaces an unbounded FlushFileBuffers.
static void wait_client_close(HANDLE p, HANDLE ev) {
    uint8_t b;
    read_all_timeout(p, &b, 1, kIoTimeoutMs, ev);
}

static bool connect_client(HANDLE p, HANDLE ev) {
    OVERLAPPED ov = {};
    ov.hEvent = ev;
    ResetEvent(ev);
    if (ConnectNamedPipe(p, &ov)) return true;
    DWORD e = GetLastError();
    if (e == ERROR_PIPE_CONNECTED) return true;
    if (e != ERROR_IO_PENDING) return false;
    DWORD n = 0;
    return GetOverlappedResult(p, &ov, &n, TRUE) != 0;  // server_stop connects once per thread
}

static DWORD WINAPI server_thread(void* arg) {
    wchar_t* path = (wchar_t*)mem_alloc((ABT_MAX_PATH_CHARS + 1) * sizeof(wchar_t));
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE p = (HANDLE)arg;  // thread 0 receives the first instance
    while (!g_stop && path && ev) {
        if (p == INVALID_HANDLE_VALUE || !p) p = create_instance(false);
        if (p == INVALID_HANDLE_VALUE) { Sleep(100); continue; }
        if (connect_client(p, ev) && !g_stop) {
            handle_client(p, path, ev);
            wait_client_close(p, ev);
        }
        DisconnectNamedPipe(p);
        CloseHandle(p);
        p = INVALID_HANDLE_VALUE;
    }
    if (p != INVALID_HANDLE_VALUE && p) CloseHandle(p);
    if (ev) CloseHandle(ev);
    mem_free(path);
    return 0;
}

bool server_start(int threads) {
    pipe_name(g_name, _countof(g_name));
    if (!make_sd()) {  // never fall back to the default DACL
        logf("pipe security descriptor failed: %lu", GetLastError());
        return false;
    }
    g_first = create_instance(true);
    if (g_first == INVALID_HANDLE_VALUE) {
        logf("pipe %ls unavailable (%lu): another agent is running?", g_name, GetLastError());
        return false;
    }
    if (threads < 1) threads = 1;
    if (threads > (int)_countof(g_threads)) threads = (int)_countof(g_threads);
    for (int i = 0; i < threads; ++i) {
        g_threads[g_nthreads] = CreateThread(nullptr, 64 << 10, server_thread, i == 0 ? g_first : INVALID_HANDLE_VALUE,
                                             STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (g_threads[g_nthreads]) ++g_nthreads;
    }
    logf("pipe server: %ls, %d threads", g_name, g_nthreads);
    return g_nthreads > 0;
}

void server_stop() {
    g_stop = true;
    // Unblock ConnectNamedPipe in every thread by connecting once per thread.
    for (int i = 0; i < g_nthreads; ++i) {
        HANDLE h = CreateFileW(g_name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    // Every pipe read/write has a timeout and the pool has already cancelled queued jobs, so
    // threads finish within a few seconds. If one still runs, keep what it may touch alive.
    bool all_done = !g_nthreads ||
                    WaitForMultipleObjects((DWORD)g_nthreads, g_threads, TRUE, 8000) != WAIT_TIMEOUT;
    if (!all_done) logf("pipe server: threads still busy at shutdown");
    for (int i = 0; i < g_nthreads; ++i) CloseHandle(g_threads[i]);
    g_nthreads = 0;
    if (all_done && g_sd) { LocalFree(g_sd); g_sd = nullptr; }
}

}  // namespace abt
