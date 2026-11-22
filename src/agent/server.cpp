// Named-pipe server: answers the shell extension (thumbnails, preview pane) and the CLI.
// Supports persistent keep-alive connections and reuses pipe instances across clients.
#include "agent.h"
#include <sddl.h>
#include <objbase.h>

namespace abt {

Stats g_stats;

static HANDLE g_threads[32];
static int g_nthreads = 0;
static volatile bool g_stop = false;
static HANDLE g_stop_event = nullptr;
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
                            // NOLINTNEXTLINE(misc-redundant-expression): three of these are 0, named for clarity
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                            PIPE_UNLIMITED_INSTANCES, 512 * 1024, 64 * 1024, 0, g_sd ? &sa : nullptr);
}

// Fits src (RefPixBuf) into (bw, bh) without enlarging. If no resize is needed, returns src directly
// (zero-copy); otherwise allocates a resized RefPixBuf and releases src.
static RefPixBuf* fit_to_box(RefPixBuf* src, int bw, int bh) {
    if (!src || !src->px || bw <= 0 || bh <= 0) return src;
    double s = (double)bw / src->w, s2 = (double)bh / src->h;
    if (s2 < s) s = s2;
    if (s >= 1.0) return src;  // never enlarge here
    int dw = (int)(src->w * s + 0.5), dh = (int)(src->h * s + 0.5);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    abt_image r = {};
    if (abt_resize(src->px, src->w, src->h, (size_t)src->w * 4, dw, dh, &r) == ABT_OK && r.px) {
        RefPixBuf* dst = pixbuf_alloc(r.w, r.h, src->has_alpha);
        if (dst) {
            memcpy(dst->px, r.px, (size_t)r.w * (size_t)r.h * 4u);
            abt_image_free(&r);
            pixbuf_release(src);
            return dst;
        }
        abt_image_free(&r);
    }
    return src;
}

static uint32_t flags_to_source(uint16_t f) {
    uint32_t s = ABT_SRC_CACHE;
    if (f & CF_EMBEDDED) s |= ABT_SRC_EMBEDDED;
    if (f & CF_RENDERED) s |= ABT_SRC_RENDERED;
    if (f & CF_SCALED) s |= ABT_SRC_SCALED;
    return s;
}

// ---- network shares that stopped answering (D-033)
struct DownShare {
    wchar_t key[80];
    ULONGLONG until;
};
static DownShare g_down[8];
static SRWLOCK g_down_lock = SRWLOCK_INIT;

static bool prefix_ieq(const wchar_t* s, const wchar_t* prefix, int len) {
    return CompareStringOrdinal(s, len, prefix, len, TRUE) == CSTR_EQUAL;
}

// "server\share" or "X:" (mapped drive); empty for local paths.
static void share_key(const wchar_t* path, wchar_t* out, size_t cap) {
    out[0] = 0;
    if (cap < 3 || !is_slow_path(path)) return;
    const wchar_t* p = path;
    if (!wcsncmp(p, L"\\\\?\\", 4) && prefix_ieq(p + 4, L"UNC\\", 4)) p += 8;
    else if (!wcsncmp(p, L"\\\\?\\", 4)) { lstrcpynW(out, p + 4, 3); return; }
    else if (!wcsncmp(p, L"\\\\", 2)) p += 2;
    else { lstrcpynW(out, path, 3); return; }
    const wchar_t* e = p;
    int seps = 0;
    while (*e && !(*e == L'\\' && ++seps == 2)) ++e;
    size_t n = (size_t)(e - p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n * sizeof(wchar_t));
    out[n] = 0;
}

static bool share_down(const wchar_t* key) {
    if (!key[0]) return false;
    ULONGLONG now = GetTickCount64();
    bool down = false;
    AcquireSRWLockShared(&g_down_lock);
    for (const DownShare& d : g_down)
        if (d.until > now && ieq(d.key, key)) { down = true; break; }
    ReleaseSRWLockShared(&g_down_lock);
    return down;
}

static void mark_share_down(const wchar_t* key) {
    if (!key[0]) return;
    AcquireSRWLockExclusive(&g_down_lock);
    int slot = 0;
    for (int i = 0; i < (int)_countof(g_down); ++i) {
        if (ieq(g_down[i].key, key)) { slot = i; break; }
        if (g_down[i].until < g_down[slot].until) slot = i;
    }
    lstrcpynW(g_down[slot].key, key, (int)_countof(g_down[slot].key));
    g_down[slot].until = GetTickCount64() + 30000;
    ReleaseSRWLockExclusive(&g_down_lock);
    logf("share %ls not answering: skipped for 30 s", key);
}

static bool network_error(DWORD e) {
    switch (e) {
    case ERROR_BAD_NETPATH: case ERROR_NETNAME_DELETED: case ERROR_NETWORK_UNREACHABLE:
    case ERROR_HOST_UNREACHABLE: case ERROR_UNEXP_NET_ERR: case ERROR_SEM_TIMEOUT: case ERROR_BAD_NET_NAME:
    case ERROR_NO_NET_OR_BAD_PATH: case ERROR_NOT_CONNECTED: case ERROR_CONNECTION_ABORTED:
    case ERROR_NETWORK_BUSY: case ERROR_REM_NOT_LIST:
        return true;
    default:
        return false;
    }
}

static void serve_image(const abt_req& req, const wchar_t* path, abt_resp* resp, RefPixBuf** out_pix) {
    *out_pix = nullptr;
    uint64_t t0 = qpc();
    InterlockedIncrement64(&g_stats.requests);
    wchar_t share[80];
    share_key(path, share, _countof(share));
    if (share_down(share)) {
        resp->hr = HRESULT_FROM_WIN32(ERROR_BAD_NETPATH);
        lstrcpynA(resp->reason, "network share not answering (retried in 30 s)", sizeof(resp->reason));
        InterlockedIncrement64(&g_stats.failures);
        return;
    }
    FileKey k;
    uint64_t tk = qpc();
    bool have_key = file_key(path, &k);
    DWORD key_err = have_key ? 0 : GetLastError();
    if (share[0] && ((!have_key && network_error(key_err)) || qpc_ms(tk, qpc()) > 3000.0)) mark_share_down(share);
    if (!have_key) {
        resp->hr = HRESULT_FROM_WIN32(key_err ? key_err : ERROR_FILE_NOT_FOUND);
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
        if (cache_has(k, page, edge, &view)) {
            resp->hr = (view.flags & CF_NEGATIVE) ? S_FALSE : S_OK;
            resp->info = view.info;
            lstrcpynA(resp->reason, "prefetch: cached", sizeof(resp->reason));
        } else if (Job* pj = job_submit(path, k, page, edge, edge, ABT_MODE_AUTO,
                                        req.flags & ~(ABT_RF_PREFETCH | ABT_RF_CACHE_ONLY), LANE_PREVIEW)) {
            job_release(pj);
            resp->hr = S_FALSE;
            lstrcpynA(resp->reason, "prefetch: queued", sizeof(resp->reason));
        } else {
            resp->hr = E_OUTOFMEMORY;
        }
        resp->total_len = sizeof(*resp);
        resp->elapsed_us = (uint32_t)(qpc_ms(t0, qpc()) * 1000.0);
        return;
    }
    RefPixBuf* pix = nullptr;
    if (!(req.flags & ABT_RF_NO_CACHE) && !info_only && cache_get(k, page, edge, &pix, &view)) {
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
    } else if (req.flags & ABT_RF_CACHE_ONLY) {
        // Preview pane quick first-paint: never block on a redundant 256 px render on cache miss.
        resp->hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        lstrcpynA(resp->reason, "cache miss (cache-only)", sizeof(resp->reason));
    } else {
        InterlockedIncrement64(&g_stats.misses);
        const bool slow = req.msg == ABT_MSG_THUMB && share[0] && !(req.flags & ABT_RF_FROM_PREWARM);
        Lane lane = req.msg == ABT_MSG_PREVIEW ? LANE_PREVIEW
                    : (req.flags & ABT_RF_FROM_PREWARM) ? LANE_VISIBLE
                    : slow ? LANE_FOLDER
                           : LANE_FOREGROUND;
        const uint32_t mode = info_only ? ABT_MODE_PROBE : ABT_MODE_AUTO;
        Job* j = job_submit(path, k, page, edge, edge, mode, req.flags, lane);
        if (!j) {
            resp->hr = E_OUTOFMEMORY;
        } else {
            DWORD wait = req.deadline_ms ? req.deadline_ms
                         : req.msg == ABT_MSG_PREVIEW ? 25000u
                         : slow ? 5000u
                                : 12000u;
            JobResult r = {};
            bool done = false;
            // Progressive upgrade (D-011): a thumbnail whose render is slow gets the embedded preview
            // now; the render keeps going, lands in our cache, and Explorer's entry is refreshed.
            if (req.msg == ABT_MSG_THUMB && !(req.flags & ABT_RF_FROM_PREWARM) && wait > 350) {
                uint64_t tw = qpc();
                done = !slow && job_wait(j, 350, &r);
                if (!done) {
                    Job* fast = job_submit(path, k, page, edge, edge, ABT_MODE_FAST, req.flags | ABT_RF_NO_CACHE,
                                           LANE_FOREGROUND);
                    JobResult fr = {};
                    if (fast && job_wait(fast, slow ? 2500 : 1500, &fr) && SUCCEEDED(fr.hr) && fr.pix) {
                        resp->hr = S_OK;
                        resp->info = fr.info;
                        resp->source = fr.source | ABT_SRC_UPGRADE_PENDING;
                        lstrcpynA(resp->reason, "embedded preview; sharper render on its way", sizeof(resp->reason));
                        pix = fr.pix;
                        prewarm_upgrade_when_done(j, path, edge);  // takes over our reference
                        j = nullptr;
                    } else if (fast) {
                        pixbuf_release(fr.pix);
                    }
                    if (fast) job_release(fast);
                    if (!j) goto finish;
                    // No usable embedded preview: promote our existing queued job (never spawn a duplicate).
                    if (slow) job_promote(j, LANE_FOREGROUND);
                    DWORD spent = (DWORD)qpc_ms(tw, qpc());
                    wait = wait > spent + 100 ? wait - spent : 100;
                }
            }
            if (done || job_wait(j, wait, &r)) {
                resp->hr = r.hr;
                resp->info = r.info;
                resp->source = r.source;
                memcpy(resp->reason, r.reason, sizeof(resp->reason));
                pix = r.pix;
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
    if (SUCCEEDED(resp->hr) && pix && pix->px) {
        pix = fit_to_box(pix, bw, bh);
        resp->width = (uint32_t)pix->w;
        resp->height = (uint32_t)pix->h;
        resp->pixel_len = resp->width * resp->height * 4;
        if (pix->has_alpha) resp->source |= ABT_SRC_HAS_ALPHA;
        else resp->source &= ~ABT_SRC_HAS_ALPHA;
        *out_pix = pix;
    } else {
        pixbuf_release(pix);
    }
    resp->total_len = sizeof(*resp) + resp->pixel_len;
    resp->elapsed_us = (uint32_t)(qpc_ms(t0, qpc()) * 1000.0);
    InterlockedAdd64(&g_stats.total_us, resp->elapsed_us);
    if (SUCCEEDED(resp->hr) && req.msg == ABT_MSG_THUMB &&
        !(req.flags & (ABT_RF_FROM_PREWARM | ABT_RF_NO_CACHE | ABT_RF_CACHE_ONLY)))
        prewarm_hint_file(path, edge);
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

static bool is_ascii_alpha(wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'); }

static bool acceptable_unc_tail(const wchar_t* p) {
    // Expect non-empty "server\share" with no '.', '?', '/', '\' leading server, and reject pipe / IPC$.
    if (!p[0] || p[0] == L'.' || p[0] == L'?' || p[0] == L'/' || p[0] == L'\\') return false;
    const wchar_t* s = p;
    while (*s && *s != L'\\' && *s != L'/') ++s;
    if (*s != L'\\' || s == p) return false;
    const wchar_t* sh = s + 1;
    const wchar_t* she = sh;
    while (*she && *she != L'\\' && *she != L'/') ++she;
    int sh_len = (int)(she - sh);
    if (sh_len <= 0) return false;
    if ((sh_len == 4 && prefix_ieq(sh, L"pipe", 4)) || (sh_len == 4 && prefix_ieq(sh, L"IPC$", 4))) return false;
    return true;
}

// Paths must be plain drive-letter or UNC paths: no device namespace (\\.\, \\?\GLOBALROOT, \??\),
// no named-pipe/IPC$ shares, no embedded NULs, ASCII drive letters only.
static bool acceptable_path(const wchar_t* path, uint32_t chars) {
    if (chars < 3 || wcslen(path) != chars) return false;
    if (!wcsncmp(path, L"\\\\?\\", 4)) {
        path += 4;
        if (prefix_ieq(path, L"UNC\\", 4)) return acceptable_unc_tail(path + 4);
        return is_ascii_alpha(path[0]) && path[1] == L':' && path[2] == L'\\';
    }
    if (!wcsncmp(path, L"\\\\", 2)) {
        return acceptable_unc_tail(path + 2);
    }
    return is_ascii_alpha(path[0]) && path[1] == L':' && path[2] == L'\\';
}

static const DWORD kIoTimeoutMs = 5000;

static bool handle_request(HANDLE p, abt_req req, const wchar_t* path, const Caller& caller, HANDLE ev) {
    abt_resp resp;
    init_resp(&resp, (abt_msg)req.msg);
    if (caller.low) {
        req.flags &= (ABT_RF_PREFETCH | ABT_RF_CACHE_ONLY);  // only scheduling/cache hints (D-028, D-030)
        if (req.msg != ABT_MSG_THUMB && req.msg != ABT_MSG_PREVIEW && req.msg != ABT_MSG_INFO && req.msg != ABT_MSG_PING)
            req.msg = 0;  // -> E_INVALIDARG below
    }
    if (!caller.self) req.flags &= ~ABT_RF_FROM_PREWARM;
    bool image = req.msg == ABT_MSG_THUMB || req.msg == ABT_MSG_PREVIEW || req.msg == ABT_MSG_INFO;
    if ((image || req.msg == ABT_MSG_HINT) &&
        (!acceptable_path(path, req.path_chars) || req.cx > ABT_MAX_REPLY_DIM || req.cy > ABT_MAX_REPLY_DIM ||
         req.page > 0xFFFF)) {
        resp.hr = E_INVALIDARG;
        return write_all_timeout(p, &resp, sizeof(resp), kIoTimeoutMs, ev);
    }
    RefPixBuf* pix = nullptr;
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
        int edge = req.cx ? (int)req.cx : 256;
        if (edge > 2560) edge = 2560;
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) prewarm_hint_folder(path, edge);
        else prewarm_hint_file(path, edge);
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
        serve_image(req, path, &resp, &pix);
        break;
    default:
        resp.hr = E_INVALIDARG;
        break;
    }
    // Bounded writes: a client that stops reading cannot park this thread.
    const DWORD out_timeout = kIoTimeoutMs + resp.pixel_len / 4096;  // ~4 MB/s floor for big previews
    const void* payload = text ? (const void*)text : (pix ? (const void*)pix->px : nullptr);
    bool ok = write_all_timeout(p, &resp, sizeof(resp), kIoTimeoutMs, ev);
    if (ok && resp.pixel_len && payload)
        ok = write_all_timeout(p, payload, resp.pixel_len, out_timeout, ev);
    mem_free(text);
    pixbuf_release(pix);
    return ok;
}

// Persistent keep-alive loop: serves 1 or more requests on a connected pipe instance until the
// client closes, idles out (5 s), sends a bad frame, or quits. Identifies the caller once.
static void handle_connection(HANDLE p, wchar_t* path, HANDLE ev) {
    bool identified = false;
    Caller caller = {true, false};
    while (!g_stop) {
        abt_req req;
        if (!read_all_timeout(p, &req, sizeof(req), kIoTimeoutMs, ev)) break;
        if (req.magic != ABT_MAGIC || req.version != ABT_PROTO_VERSION ||
            req.path_chars > ABT_MAX_PATH_CHARS ||
            req.total_len != sizeof(req) + req.path_chars * sizeof(wchar_t))
            break;
        if (req.path_chars && !read_all_timeout(p, path, req.path_chars * sizeof(wchar_t), kIoTimeoutMs, ev))
            break;
        path[req.path_chars] = 0;
        if (!identified) {
            caller = identify_caller(p);
            identified = true;
        }
        if (!handle_request(p, req, path, caller, ev)) break;
        if (req.msg == ABT_MSG_QUIT) break;
    }
}

static bool connect_client(HANDLE p, HANDLE ev) {
    OVERLAPPED ov = {};
    ov.hEvent = ev;
    ResetEvent(ev);
    if (ConnectNamedPipe(p, &ov)) return true;
    DWORD e = GetLastError();
    if (e == ERROR_PIPE_CONNECTED) return true;
    if (e != ERROR_IO_PENDING) return false;
    HANDLE waits[2] = {ev, g_stop_event};
    DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    DWORD n = 0;
    if (r != WAIT_OBJECT_0) {
        CancelIoEx(p, &ov);
        GetOverlappedResult(p, &ov, &n, TRUE);
        return false;
    }
    return GetOverlappedResult(p, &ov, &n, FALSE) != 0;
}

static DWORD WINAPI server_thread(void* arg) {
    // Cache reads decode JPEG with WIC on this thread (D-034).
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    wchar_t* path = (wchar_t*)mem_alloc((ABT_MAX_PATH_CHARS + 1) * sizeof(wchar_t));
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE p = (HANDLE)arg;  // thread 0 receives the first instance
    while (!g_stop && path && ev) {
        if (p == INVALID_HANDLE_VALUE || !p) p = create_instance(false);
        if (p == INVALID_HANDLE_VALUE || !p) { p = INVALID_HANDLE_VALUE; Sleep(100); continue; }
        if (connect_client(p, ev) && !g_stop) {
            handle_connection(p, path, ev);
            // Reuse the pipe instance across clients (avoids CreateNamedPipe/CloseHandle churn and
            // prevents a zero-instance gap when ServerThreads=1).
            if (!DisconnectNamedPipe(p)) {
                CloseHandle(p);
                p = INVALID_HANDLE_VALUE;
            }
        } else if (!g_stop) {
            CloseHandle(p);
            p = INVALID_HANDLE_VALUE;
        }
    }
    if (p != INVALID_HANDLE_VALUE && p) {
        DisconnectNamedPipe(p);
        CloseHandle(p);
    }
    if (ev) CloseHandle(ev);
    mem_free(path);
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

bool server_start(int threads) {
    pipe_name(g_name, _countof(g_name));
    g_stop = false;
    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop_event) return false;
    if (!make_sd()) {  // never fall back to the default DACL
        logf("pipe security descriptor failed: %lu", GetLastError());
        CloseHandle(g_stop_event); g_stop_event = nullptr;
        return false;
    }
    g_first = create_instance(true);
    if (g_first == INVALID_HANDLE_VALUE) {
        logf("pipe %ls unavailable (%lu): another agent is running?", g_name, GetLastError());
        LocalFree(g_sd); g_sd = nullptr;
        CloseHandle(g_stop_event); g_stop_event = nullptr;
        return false;
    }
    if (threads < 1) threads = 1;
    if (threads > (int)_countof(g_threads)) threads = (int)_countof(g_threads);
    g_nthreads = 0;
    bool first_handed = false;
    for (int i = 0; i < threads; ++i) {
        HANDLE pass = (!first_handed) ? g_first : INVALID_HANDLE_VALUE;
        HANDLE th = CreateThread(nullptr, 512 << 10, server_thread, pass, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (th) {
            if (!first_handed) first_handed = true;
            g_threads[g_nthreads++] = th;
        }
    }
    if (!first_handed) {
        CloseHandle(g_first);
        g_first = INVALID_HANDLE_VALUE;
    }
    if (g_nthreads == 0) {
        LocalFree(g_sd); g_sd = nullptr;
        CloseHandle(g_stop_event); g_stop_event = nullptr;
        return false;
    }
    logf("pipe server: %ls, %d threads", g_name, g_nthreads);
    return true;
}

bool server_stop() {
    g_stop = true;
    if (g_stop_event) SetEvent(g_stop_event);
    // Every pipe read/write has a timeout, ConnectNamedPipe watches g_stop_event, and the pool has
    // already cancelled queued jobs, so threads exit cleanly.
    bool all_done = !g_nthreads ||
                    WaitForMultipleObjects((DWORD)g_nthreads, g_threads, TRUE, 8000) != WAIT_TIMEOUT;
    if (!all_done) logf("pipe server: threads still busy at shutdown");
    for (int i = 0; i < g_nthreads; ++i) CloseHandle(g_threads[i]);
    g_nthreads = 0;
    if (all_done) {
        if (g_sd) { LocalFree(g_sd); g_sd = nullptr; }
        if (g_stop_event) { CloseHandle(g_stop_event); g_stop_event = nullptr; }
    }
    return all_done;
}

}  // namespace abt
