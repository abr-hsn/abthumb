#include "abt_proto.h"
#include <sddl.h>

namespace abt {

bool pipe_name(wchar_t* out, size_t cap) {
    if (!out || cap == 0) return false;
    static wchar_t cached[128];
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockShared(&lock);
    bool have = cached[0] != 0;
    if (have) lstrcpynW(out, cached, (int)cap);
    ReleaseSRWLockShared(&lock);
    if (have) return true;

    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    // Hash the user SID (FNV-1a 64) so the name is per user without exposing the SID.
    // Fail closed if the token/SID cannot be queried rather than caching a predictable fallback.
    uint64_t h = 1469598103934665603ull;
    bool got_sid = false;
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        uint8_t buf[256];
        DWORD len = 0;
        if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &len)) {
            PSID sid = ((TOKEN_USER*)buf)->User.Sid;
            if (sid && IsValidSid(sid)) {
                DWORD sl = GetLengthSid(sid);
                for (DWORD i = 0; i < sl; ++i) { h ^= ((uint8_t*)sid)[i]; h *= 1099511628211ull; }
                got_sid = true;
            }
        }
        CloseHandle(tok);
    }
    if (!got_sid) return false;
    wchar_t name[128];
    _snwprintf_s(name, _countof(name), _TRUNCATE, L"\\\\.\\pipe\\abthumb.v1.%lu.%016llx", session, (unsigned long long)h);
    AcquireSRWLockExclusive(&lock);
    lstrcpynW(cached, name, _countof(cached));
    ReleaseSRWLockExclusive(&lock);
    lstrcpynW(out, name, (int)cap);
    return true;
}

bool write_all(HANDLE h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    while (n) {
        DWORD chunk = n > (1u << 30) ? (1u << 30) : (DWORD)n, w = 0;
        if (!WriteFile(h, b, chunk, &w, nullptr) || !w) return false;
        b += w;
        n -= w;
    }
    return true;
}

bool read_all(HANDLE h, void* p, size_t n) {
    uint8_t* b = (uint8_t*)p;
    while (n) {
        DWORD chunk = n > (1u << 30) ? (1u << 30) : (DWORD)n, r = 0;
        if (!ReadFile(h, b, chunk, &r, nullptr) || !r) return false;
        b += r;
        n -= r;
    }
    return true;
}

static bool io_timeout(HANDLE h, void* p, size_t n, DWORD timeout_ms, HANDLE ev, bool write) {
    uint8_t* b = (uint8_t*)p;
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (n) {
        OVERLAPPED ov = {};
        ov.hEvent = ev;
        ResetEvent(ev);
        DWORD chunk = n > (1u << 30) ? (1u << 30) : (DWORD)n, done = 0;
        BOOL ok = write ? WriteFile(h, b, chunk, nullptr, &ov) : ReadFile(h, b, chunk, nullptr, &ov);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            ULONGLONG now = GetTickCount64();
            DWORD wait = now >= deadline ? 0 : (DWORD)(deadline - now);
            if (WaitForSingleObject(ev, wait) != WAIT_OBJECT_0) {
                CancelIoEx(h, &ov);
                GetOverlappedResult(h, &ov, &done, TRUE);
                return false;
            }
        }
        if (!GetOverlappedResult(h, &ov, &done, FALSE) || !done) return false;
        b += done;
        n -= done;
    }
    return true;
}

bool read_all_timeout(HANDLE h, void* p, size_t n, DWORD t, HANDLE ev) { return io_timeout(h, p, n, t, ev, false); }
bool write_all_timeout(HANDLE h, const void* p, size_t n, DWORD t, HANDLE ev) {
    return io_timeout(h, (void*)p, n, t, ev, true);
}

void init_req(abt_req* r, abt_msg msg, uint32_t path_chars) {
    memset(r, 0, sizeof(*r));
    r->magic = ABT_MAGIC;
    r->version = ABT_PROTO_VERSION;
    r->msg = msg;
    r->path_chars = path_chars;
    r->total_len = (uint32_t)(sizeof(abt_req) + path_chars * sizeof(wchar_t));
}

void init_resp(abt_resp* r, abt_msg msg) {
    memset(r, 0, sizeof(*r));
    r->magic = ABT_MAGIC;
    r->version = ABT_PROTO_VERSION;
    r->msg = msg;
    r->total_len = sizeof(abt_resp);
    r->hr = E_FAIL;
}

// Own user SID cached once per process for fast server_trusted checks.
static uint8_t g_self_sid[SECURITY_MAX_SID_SIZE];
static bool g_have_self_sid = false;
static INIT_ONCE g_self_sid_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_self_sid(PINIT_ONCE, PVOID, PVOID*) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return FALSE;
    uint8_t buf[256];
    DWORD len = 0;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &len)) {
        PSID sid = ((TOKEN_USER*)buf)->User.Sid;
        if (sid && IsValidSid(sid)) {
            DWORD sl = GetLengthSid(sid);
            if (sl <= sizeof(g_self_sid)) {
                memcpy(g_self_sid, sid, sl);
                g_have_self_sid = true;
            }
        }
    }
    CloseHandle(tok);
    return g_have_self_sid ? TRUE : FALSE;
}

static SRWLOCK g_trust_lock = SRWLOCK_INIT;
static ULONG g_trusted_pid = 0;
static FILETIME g_trusted_ctime = {};

// Anyone can create a pipe with our (predictable) name before the agent does. Only talk to a
// server running as our user at medium integrity or above. A low-integrity caller (prevhost)
// cannot open the server process; it then relies on the reply validation below.
static bool server_trusted(HANDLE pipe) {
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &pid)) return false;
    if (pid == GetCurrentProcessId()) return true;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return is_below_medium_integrity();

    FILETIME ct = {}, et = {}, kt = {}, ut = {};
    bool have_ct = GetProcessTimes(proc, &ct, &et, &kt, &ut) != 0;
    if (have_ct) {
        AcquireSRWLockShared(&g_trust_lock);
        bool hit = g_trusted_pid == pid && g_trusted_ctime.dwLowDateTime == ct.dwLowDateTime &&
                   g_trusted_ctime.dwHighDateTime == ct.dwHighDateTime;
        ReleaseSRWLockShared(&g_trust_lock);
        if (hit) {
            CloseHandle(proc);
            return true;
        }
    }

    InitOnceExecuteOnce(&g_self_sid_once, init_self_sid, nullptr, nullptr);
    bool ok = false;
    HANDLE stok = nullptr;
    if (g_have_self_sid && OpenProcessToken(proc, TOKEN_QUERY, &stok)) {
        uint8_t sb[128];
        DWORD len = 0;
        ok = !token_below_medium_integrity(stok) && GetTokenInformation(stok, TokenUser, sb, sizeof(sb), &len) &&
             EqualSid(((TOKEN_USER*)sb)->User.Sid, (PSID)g_self_sid);
        CloseHandle(stok);
    }
    CloseHandle(proc);
    if (ok && have_ct) {
        AcquireSRWLockExclusive(&g_trust_lock);
        g_trusted_pid = pid;
        g_trusted_ctime = ct;
        ReleaseSRWLockExclusive(&g_trust_lock);
    }
    return ok;
}

// Small idle named-pipe handle pool (up to 8 handles, 4s idle TTL). Each entry owns an already
// server_trusted-verified pipe handle and its overlapped event handle, so repeat calls avoid
// CreateFileW, server_trusted, and CreateEventW/CloseHandle.
static constexpr int kPipePoolCap = 8;
static constexpr ULONGLONG kPipeIdleTtlMs = 4000;

struct PooledPipe {
    HANDLE h;
    HANDLE ev;
    ULONGLONG last_used;
};

static PooledPipe g_pipe_pool[kPipePoolCap];
static int g_pipe_pool_n = 0;
static SRWLOCK g_pipe_pool_lock = SRWLOCK_INIT;

static void close_pooled(HANDLE h, HANDLE ev) {
    if (ev) CloseHandle(ev);
    if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

static bool pipe_pool_pop(HANDLE* out_h, HANDLE* out_ev) {
    PooledPipe stale[kPipePoolCap] = {};
    int nstale = 0;
    bool got = false;
    ULONGLONG now = GetTickCount64();

    AcquireSRWLockExclusive(&g_pipe_pool_lock);
    int keep = 0;
    for (int i = 0; i < g_pipe_pool_n; ++i) {
        if (now - g_pipe_pool[i].last_used > kPipeIdleTtlMs) {
            stale[nstale++] = g_pipe_pool[i];
        } else {
            g_pipe_pool[keep++] = g_pipe_pool[i];
        }
    }
    g_pipe_pool_n = keep;
    if (g_pipe_pool_n > 0) {
        PooledPipe p = g_pipe_pool[--g_pipe_pool_n];
        *out_h = p.h;
        *out_ev = p.ev;
        got = true;
    }
    ReleaseSRWLockExclusive(&g_pipe_pool_lock);

    for (int i = 0; i < nstale; ++i) close_pooled(stale[i].h, stale[i].ev);
    return got;
}

static void pipe_pool_push(HANDLE h, HANDLE ev) {
    PooledPipe stale[kPipePoolCap + 1] = {};
    int nstale = 0;
    ULONGLONG now = GetTickCount64();

    AcquireSRWLockExclusive(&g_pipe_pool_lock);
    int keep = 0;
    for (int i = 0; i < g_pipe_pool_n; ++i) {
        if (now - g_pipe_pool[i].last_used > kPipeIdleTtlMs) {
            stale[nstale++] = g_pipe_pool[i];
        } else {
            g_pipe_pool[keep++] = g_pipe_pool[i];
        }
    }
    g_pipe_pool_n = keep;
    if (g_pipe_pool_n == kPipePoolCap) {
        stale[nstale++] = g_pipe_pool[0];
        memmove(&g_pipe_pool[0], &g_pipe_pool[1], sizeof(PooledPipe) * (kPipePoolCap - 1));
        --g_pipe_pool_n;
    }
    g_pipe_pool[g_pipe_pool_n++] = {h, ev, now};
    ReleaseSRWLockExclusive(&g_pipe_pool_lock);

    for (int i = 0; i < nstale; ++i) close_pooled(stale[i].h, stale[i].ev);
}

static HRESULT connect_pipe(const wchar_t* name, ULONGLONG deadline, HANDLE* out_h, HANDLE* out_ev) {
    HANDLE h = INVALID_HANDLE_VALUE;
    for (;;) {
        h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        DWORD e = GetLastError();
        if (e != ERROR_PIPE_BUSY) return HRESULT_FROM_WIN32(e);
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        DWORD wait = (DWORD)(deadline - now);
        if (wait == 0) wait = 1;
        if (!WaitNamedPipeW(name, wait)) {
            DWORD we = GetLastError();
            if (we != ERROR_SEM_TIMEOUT && we != ERROR_FILE_NOT_FOUND) Sleep(5);  // avoid 100% CPU spin
            if (we == ERROR_FILE_NOT_FOUND) return HRESULT_FROM_WIN32(we);
        }
    }
    if (!server_trusted(h)) {
        CloseHandle(h);
        return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    }
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) {
        DWORD e = GetLastError();
        CloseHandle(h);
        return HRESULT_FROM_WIN32(e ? e : ERROR_OUTOFMEMORY);
    }
    *out_h = h;
    *out_ev = ev;
    return S_OK;
}

HRESULT pipe_call(const abt_req& req, const wchar_t* path, DWORD timeout_ms, abt_resp* resp, uint8_t** pixels) {
    if (!resp || !pixels || req.path_chars > ABT_MAX_PATH_CHARS || (req.path_chars && !path)) return E_INVALIDARG;
    *pixels = nullptr;
    wchar_t name[128];
    if (!pipe_name(name, _countof(name))) return E_FAIL;

    const size_t plen = (size_t)req.path_chars * sizeof(wchar_t);
    uint8_t stackbuf[sizeof(abt_req) + 1024];
    uint8_t* msg = stackbuf;
    if (sizeof(abt_req) + plen > sizeof(stackbuf)) msg = (uint8_t*)mem_alloc(sizeof(abt_req) + plen);
    if (!msg) return E_OUTOFMEMORY;
    memcpy(msg, &req, sizeof(req));
    if (plen) memcpy(msg + sizeof(req), path, plen);

    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    auto left = [&]() -> DWORD {
        ULONGLONG now = GetTickCount64();
        return now >= deadline ? 0 : (DWORD)(deadline - now);
    };

    HRESULT hr = E_FAIL;
    for (int attempt = 0; attempt < 2; ++attempt) {
        HANDLE h = INVALID_HANDLE_VALUE;
        HANDLE ev = nullptr;
        bool from_pool = (attempt == 0) && pipe_pool_pop(&h, &ev);
        if (!from_pool) {
            hr = connect_pipe(name, deadline, &h, &ev);
            if (FAILED(hr)) break;
        }
        bool got_hdr = write_all_timeout(h, msg, sizeof(req) + plen, left(), ev) &&
                       read_all_timeout(h, resp, sizeof(*resp), left(), ev);
        if (!got_hdr) {
            CloseHandle(ev);
            CloseHandle(h);
            if (from_pool && left() > 0) continue;  // stale pooled handle or single-request server: retry once
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            break;
        }
        // The response may come from anything that managed to open a pipe with our name, and
        // callers turn width/height into DIB sizes and print strings with %s (D-028).
        resp->reason[sizeof(resp->reason) - 1] = 0;
        resp->info.creator[sizeof(resp->info.creator) - 1] = 0;
        resp->info.version[sizeof(resp->info.version) - 1] = 0;
        const uint64_t want = (uint64_t)resp->width * resp->height * 4u;
        const uint64_t expect_total = (uint64_t)sizeof(*resp) + resp->pixel_len;
        if (resp->magic != ABT_MAGIC || resp->version != ABT_PROTO_VERSION || resp->msg != req.msg ||
            resp->pixel_len > ABT_MAX_PIXEL_BYTES || resp->total_len != expect_total ||
            resp->width > ABT_MAX_REPLY_DIM || resp->height > ABT_MAX_REPLY_DIM ||
            (resp->pixel_len && (resp->pixel_len != want || !resp->width || !resp->height))) {
            CloseHandle(ev);
            CloseHandle(h);
            hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            break;
        }
        bool keep = req.msg != ABT_MSG_QUIT;
        if (resp->pixel_len) {
            uint8_t* px = (uint8_t*)mem_alloc(resp->pixel_len);
            if (!px) {
                hr = E_OUTOFMEMORY;
                keep = false;
            } else if (!read_all_timeout(h, px, resp->pixel_len, left(), ev)) {
                mem_free(px);
                hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                keep = false;
            } else {
                *pixels = px;
                hr = resp->hr;
            }
        } else {
            hr = resp->hr;
        }
        if (keep) {
            pipe_pool_push(h, ev);
        } else {
            CloseHandle(ev);
            CloseHandle(h);
        }
        break;
    }

    if (msg != stackbuf) mem_free(msg);
    return hr;
}

}  // namespace abt
