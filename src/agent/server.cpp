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
    return CreateNamedPipeW(g_name, PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
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
    CacheEntryView view = {};
    if (!(req.flags & ABT_RF_NO_CACHE) && !info_only && cache_get(k, req.page, edge, img, &view)) {
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
        Job* j = job_submit(path, k, req.page, edge, edge, info_only ? ABT_MODE_PROBE : ABT_MODE_AUTO, req.flags, lane);
        if (!j) {
            resp->hr = E_OUTOFMEMORY;
        } else {
            DWORD wait = req.deadline_ms ? req.deadline_ms : (req.msg == ABT_MSG_PREVIEW ? 25000u : 12000u);
            JobResult r;
            if (job_wait(j, wait, &r)) {
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
    if (SUCCEEDED(resp->hr) && img->px) {
        fit_to_box(img, bw, bh);
        resp->width = (uint32_t)img->w;
        resp->height = (uint32_t)img->h;
        resp->pixel_len = resp->width * resp->height * 4;
    } else {
        abt_image_free(img);
    }
    resp->total_len = sizeof(*resp) + resp->pixel_len;
    resp->elapsed_us = (uint32_t)(qpc_ms(t0, qpc()) * 1000.0);
    InterlockedAdd64(&g_stats.total_us, resp->elapsed_us);
    if (req.msg == ABT_MSG_THUMB && !(req.flags & ABT_RF_FROM_PREWARM)) prewarm_hint_file(path, edge);
}

static void handle_client(HANDLE p, wchar_t* path) {
    abt_req req;
    if (!read_all(p, &req, sizeof(req)) || req.magic != ABT_MAGIC || req.version != ABT_PROTO_VERSION ||
        req.path_chars > ABT_MAX_PATH_CHARS)
        return;
    if (req.path_chars && !read_all(p, path, req.path_chars * sizeof(wchar_t))) return;
    path[req.path_chars] = 0;
    abt_resp resp;
    init_resp(&resp, (abt_msg)req.msg);
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
    case ABT_MSG_HINT:
        prewarm_hint_file(path, req.cx ? (int)req.cx : 256);
        resp.hr = S_OK;
        break;
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
    if (write_all(p, &resp, sizeof(resp)) && resp.pixel_len) write_all(p, text ? (void*)text : (void*)img.px, resp.pixel_len);
    mem_free(text);
    abt_image_free(&img);
}

static DWORD WINAPI server_thread(void* arg) {
    wchar_t* path = (wchar_t*)mem_alloc((ABT_MAX_PATH_CHARS + 1) * sizeof(wchar_t));
    HANDLE p = (HANDLE)arg;  // thread 0 receives the first instance
    while (!g_stop && path) {
        if (p == INVALID_HANDLE_VALUE || !p) p = create_instance(false);
        if (p == INVALID_HANDLE_VALUE) { Sleep(100); continue; }
        BOOL connected = ConnectNamedPipe(p, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected && !g_stop) {
            handle_client(p, path);
            FlushFileBuffers(p);  // the client must read everything before we disconnect
        }
        DisconnectNamedPipe(p);
        CloseHandle(p);
        p = INVALID_HANDLE_VALUE;
    }
    mem_free(path);
    return 0;
}

bool server_start(int threads) {
    pipe_name(g_name, _countof(g_name));
    if (!make_sd()) logf("pipe security descriptor failed: %lu", GetLastError());
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
    for (int i = 0; i < g_nthreads; ++i) {
        if (WaitForSingleObject(g_threads[i], 2000) != WAIT_OBJECT_0) CancelSynchronousIo(g_threads[i]);
        CloseHandle(g_threads[i]);
    }
    g_nthreads = 0;
    if (g_sd) { LocalFree(g_sd); g_sd = nullptr; }
}

}  // namespace abt
